// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "SpatialReceiver.h"

#include "EngineMinimal.h"
#include "EntityRegistry.h"
#include "GameFramework/PlayerController.h"
#include "SpatialActorChannel.h"
#include "SpatialNetConnection.h"
#include "SpatialPackageMapClient.h"
#include "SpatialInterop.h"
#include "SpatialSender.h"
#include "Utils/ComponentReader.h"
#include "Utils/RepLayoutUtils.h"

#include "Schema/UnrealMetadata.h"
#include "Schema/DynamicComponent.h"

#include "SpatialConstants.h"

template <typename T>
T* GetComponentData(USpatialReceiver& Receiver, Worker_EntityId EntityId)
{
	for (PendingAddComponentWrapper& PendingAddComponent : Receiver.PendingAddComponents)
	{
		if (PendingAddComponent.EntityId == EntityId && PendingAddComponent.ComponentId == T::ComponentId)
		{
			return static_cast<T*>(PendingAddComponent.Data.get());
		}
	}

	return nullptr;
}

void USpatialReceiver::Init(USpatialNetDriver* NetDriver)
{
	this->NetDriver = NetDriver;
	this->PackageMap = NetDriver->PackageMap;
	this->World = NetDriver->GetWorld();
}

void USpatialReceiver::OnCriticalSection(bool InCriticalSection)
{
	if (InCriticalSection)
	{
		EnterCriticalSection();
	}
	else
	{
		LeaveCriticalSection();
	}
}

void USpatialReceiver::EnterCriticalSection()
{
	UE_LOG(LogTemp, Log, TEXT("SpatialReceiver: Entering critical section."));
	check(!bInCriticalSection);
	bInCriticalSection = true;
}

void USpatialReceiver::LeaveCriticalSection()
{
	UE_LOG(LogTemp, Log, TEXT("SpatialReceiver: Leaving critical section."));
	check(bInCriticalSection);

	// Add entities.
	for (Worker_EntityId& PendingAddEntity : PendingAddEntities)
	{
		CreateActor(PendingAddEntity);
	}

	// Remove entities.
	for (Worker_EntityId& PendingRemoveEntity : PendingRemoveEntities)
	{
		RemoveActor(PendingRemoveEntity);
	}

	// Mark that we've left the critical section.
	bInCriticalSection = false;
	PendingAddEntities.Empty();
	PendingAddComponents.Empty();
	PendingRemoveEntities.Empty();
}

void USpatialReceiver::OnAddEntity(Worker_AddEntityOp& Op)
{
	UE_LOG(LogTemp, Log, TEXT("PipelineBlock: AddEntity: %lld"), Op.entity_id);
	check(bInCriticalSection);

	PendingAddEntities.Emplace(Op.entity_id);
}

void USpatialReceiver::OnAddComponent(Worker_AddComponentOp& Op)
{
	UE_LOG(LogTemp, Log, TEXT("SpatialReceiver: AddComponent component ID: %u entity ID: %lld"),
		Op.data.component_id, Op.entity_id);

	check(bInCriticalSection);

	std::shared_ptr<Component> Data;

	switch (Op.data.component_id)
	{
	case ENTITY_ACL_COMPONENT_ID:
		Data = std::make_shared<Component>(EntityAcl(Op.data));
		break;
	case METADATA_COMPONENT_ID:
		Data = std::make_shared<Component>(Metadata(Op.data));
		break;
	case POSITION_COMPONENT_ID:
		Data = std::make_shared<Component>(Position(Op.data));
		break;
	case PERSISTENCE_COMPONENT_ID:
		Data = std::make_shared<Component>(Persistence(Op.data));
		break;
	case UNREAL_METADATA_COMPONENT_ID:
		Data = std::make_shared<Component>(UnrealMetadata(Op.data));
		break;
	default:
		Data = std::make_shared<Component>(DynamicComponent(Op.data));
		break;
	}

	PendingAddComponents.Emplace(Op.entity_id, Op.data.component_id, Data);
}

void USpatialReceiver::OnRemoveEntity(Worker_RemoveEntityOp& Op)
{
	UE_LOG(LogTemp, Log, TEXT("CAPIPipelineBlock: RemoveEntity: %lld"), Op.entity_id);

	if (bInCriticalSection)
	{
		PendingRemoveEntities.Emplace(Op.entity_id);
	}
	else
	{
		RemoveActor(Op.entity_id);
	}
}



void USpatialReceiver::CreateActor(Worker_EntityId EntityId)
{
	checkf(World, TEXT("We should have a world whilst processing ops."));
	check(NetDriver);

	UEntityRegistry* EntityRegistry = NetDriver->GetEntityRegistry();
	check(EntityRegistry);

	Position* PositionComponent = GetComponentData<Position>(*this, EntityId);
	Metadata* MetadataComponent = GetComponentData<Metadata>(*this, EntityId);
	check(PositionComponent && MetadataComponent);

	AActor* EntityActor = EntityRegistry->GetActorFromEntityId(EntityId);
	UE_LOG(LogTemp, Log, TEXT("!!! Checked out entity with entity ID %lld"), EntityId);

	if (EntityActor)
	{
		UClass* ActorClass = GetNativeEntityClass(MetadataComponent);

		// Option 1
		UE_LOG(LogTemp, Log, TEXT("Entity for core actor %s has been checked out on the worker which spawned it."), *EntityActor->GetName());

		UnrealMetadata* UnrealMetadataComponent = GetComponentData<UnrealMetadata>(*this, EntityId);
		check(UnrealMetadataComponent);

		USpatialPackageMapClient* PackageMap = Cast<USpatialPackageMapClient>(NetDriver->GetSpatialOSNetConnection()->PackageMap);
		check(PackageMap);

		SubobjectToOffsetMap SubobjectNameToOffset;
		for (auto& Pair : UnrealMetadataComponent->SubobjectNameToOffset)
		{
			SubobjectNameToOffset.Add(Pair.Key, Pair.Value);
		}

		FNetworkGUID NetGUID = PackageMap->ResolveEntityActor(EntityActor, EntityId, SubobjectNameToOffset);
		UE_LOG(LogTemp, Log, TEXT("Received create entity response op for %lld"), EntityId);
	}
	else
	{
		UClass* ActorClass = GetNativeEntityClass(MetadataComponent);

		if(ActorClass == nullptr)
		{
			return;
		}

		// Initial Singleton Actor replication is handled with USpatialInterop::LinkExistingSingletonActors
		//if (NetDriver->IsServer() && Interop->IsSingletonClass(ActorClass))
		//{
		//	return;
		//}

		UNetConnection* Connection = nullptr;
		UnrealMetadata* UnrealMetadataComponent = GetComponentData<UnrealMetadata>(*this, EntityId);
		check(UnrealMetadataComponent);
		bool bDoingDeferredSpawn = false;

		// If we're checking out a player controller, spawn it via "USpatialNetDriver::AcceptNewPlayer"
		if (NetDriver->IsServer() && ActorClass->IsChildOf(APlayerController::StaticClass()))
		{
			checkf(!UnrealMetadataComponent->OwnerWorkerId.IsEmpty(), TEXT("A player controller entity must have an owner worker ID."));
			FString URLString = FURL().ToString();
			FString OwnerWorkerId = UnrealMetadataComponent->OwnerWorkerId;
			URLString += TEXT("?workerId=") + OwnerWorkerId;
			Connection = NetDriver->AcceptNewPlayer(FURL(nullptr, *URLString, TRAVEL_Absolute), true);
			check(Connection);
			EntityActor = Connection->PlayerController;
		}
		else
		{
			// Either spawn the actor or get it from the level if it has a persistent name.
			if (UnrealMetadataComponent->StaticPath.IsEmpty())
			{
				UE_LOG(LogTemp, Log, TEXT("!!! Spawning a native dynamic %s whilst checking out an entity."), *ActorClass->GetFullName());
				EntityActor = SpawnNewEntity(PositionComponent, ActorClass, true);
				bDoingDeferredSpawn = true;
			}
			else
			{
				FString FullPath = UnrealMetadataComponent->StaticPath;
				UE_LOG(LogTemp, Log, TEXT("!!! Searching for a native static actor %s of class %s in the persistent level whilst checking out an entity."), *FullPath, *ActorClass->GetName());
				EntityActor = FindObject<AActor>(World, *FullPath);
			}
			check(EntityActor);

			// Get the net connection for this actor.
			if (NetDriver->IsServer())
			{
				// TODO(David): Currently, we just create an actor channel on the "catch-all" connection, then create a new actor channel once we check out the player controller
				// and create a new connection. This is fine due to lazy actor channel creation in USpatialNetDriver::ServerReplicateActors. However, the "right" thing to do
				// would be to make sure to create anything which depends on the PlayerController _after_ the PlayerController's connection is set up so we can use the right
				// one here.
				Connection = NetDriver->GetSpatialOSNetConnection();
			}
			else
			{
				Connection = NetDriver->GetSpatialOSNetConnection();
			}
		}

		// Add to entity registry. 
		EntityRegistry->AddToRegistry(EntityId, EntityActor);

		// Set up actor channel.
		USpatialPackageMapClient* PackageMap = Cast<USpatialPackageMapClient>(Connection->PackageMap);
		USpatialActorChannel* Channel = Cast<USpatialActorChannel>(Connection->CreateChannel(CHTYPE_Actor, NetDriver->IsServer()));
		check(Channel);

		if (bDoingDeferredSpawn)
		{
			FVector InitialLocation = Coordinates::ToFVector(PositionComponent->Coords);
			FVector SpawnLocation = FRepMovement::RebaseOntoLocalOrigin(InitialLocation, World->OriginLocation);
			EntityActor->FinishSpawning(FTransform(FRotator::ZeroRotator, SpawnLocation));
		}

		SubobjectToOffsetMap SubobjectNameToOffset;
		for (auto& Pair : UnrealMetadataComponent->SubobjectNameToOffset)
		{
			SubobjectNameToOffset.Add(Pair.Key, Pair.Value);
		}

		PackageMap->ResolveEntityActor(EntityActor, EntityId, SubobjectNameToOffset);
		Channel->SetChannelActor(EntityActor);

		// Apply initial replicated properties.
		// This was moved to after FinishingSpawning because components existing only in blueprints aren't added until spawning is complete
		// Potentially we could split out the initial actor state and the initial component state
		for (PendingAddComponentWrapper& PendingAddComponent : PendingAddComponents)
		{
			if (PendingAddComponent.EntityId == EntityId && PendingAddComponent.Data && PendingAddComponent.Data->bIsDynamic)
			{
				ApplyComponentData(EntityId, *static_cast<DynamicComponent*>(PendingAddComponent.Data.get())->Data, Channel);
			}
		}

		// Update interest on the entity's components after receiving initial component data (so Role and RemoteRole are properly set).
		//NetDriver->GetSpatialInterop()->SendComponentInterests(Channel, EntityId.ToSpatialEntityId());

		// This is a bit of a hack unfortunately, among the core classes only PlayerController implements this function and it requires
		// a player index. For now we don't support split screen, so the number is always 0.
		if (NetDriver->ServerConnection)
		{
			if (EntityActor->IsA(APlayerController::StaticClass()))
			{
				uint8 PlayerIndex = 0;
				// FInBunch takes size in bits not bytes
				FInBunch Bunch(NetDriver->ServerConnection, &PlayerIndex, sizeof(PlayerIndex) * 8);
				EntityActor->OnActorChannelOpen(Bunch, NetDriver->ServerConnection);
			}
			else
			{
				FInBunch Bunch(NetDriver->ServerConnection);
				EntityActor->OnActorChannelOpen(Bunch, NetDriver->ServerConnection);
			}

			// Call PostNetInit on client only.
			EntityActor->PostNetInit();
		}
	}
}

void USpatialReceiver::RemoveActor(Worker_EntityId EntityId)
{
	AActor* Actor = NetDriver->GetEntityRegistry()->GetActorFromEntityId(EntityId);

	UE_LOG(LogTemp, Log, TEXT("CAPIPipelineBlock: Remove Actor: %s %lld"), Actor ? *Actor->GetName() : TEXT("nullptr"), EntityId);

	// Actor already deleted (this worker was most likely authoritative over it and deleted it earlier).
	if (!Actor || Actor->IsPendingKill())
	{
		CleanupDeletedEntity(EntityId);
		return;
	}

	if (APlayerController* PC = Cast<APlayerController>(Actor))
	{
		// Force APlayerController::DestroyNetworkActorHandled to return false
		PC->Player = nullptr;
	}

	// Workaround for camera loss on handover: prevent UnPossess() (non-authoritative destruction of pawn, while being authoritative over the controller)
	// TODO: Check how AI controllers are affected by this (UNR-430)
	// TODO: This should be solved properly by working sets (UNR-411)
	if (APawn* Pawn = Cast<APawn>(Actor))
	{
		AController* Controller = Pawn->Controller;

		if (Controller && Controller->HasAuthority())
		{
			Pawn->Controller = nullptr;
		}
	}

	// Destruction of actors can cause the destruction of associated actors (eg. Character > Controller). Actor destroy
	// calls will eventually find their way into USpatialActorChannel::DeleteEntityIfAuthoritative() which checks if the entity
	// is currently owned by this worker before issuing an entity delete request. If the associated entity is still authoritative 
	// on this server, we need to make sure this worker doesn't issue an entity delete request, as this entity is really 
	// transitioning to the same server as the actor we're currently operating on, and is just a few frames behind. 
	// We make the assumption that if we're destroying actors here (due to a remove entity op), then this is only due to two
	// situations;
	// 1. Actor's entity has been transitioned to another server
	// 2. The Actor was deleted on another server
	// In neither situation do we want to delete associated entities, so prevent them from being issued.
	// TODO: fix this with working sets (UNR-411)
	//NetDriver->GetSpatialInterop()->StartIgnoringAuthoritativeDestruction();
	if (!World->DestroyActor(Actor, true))
	{
		UE_LOG(LogTemp, Error, TEXT("World->DestroyActor failed on RemoveActor %s %lld"), *Actor->GetName(), EntityId);
	}
	//NetDriver->GetSpatialInterop()->StopIgnoringAuthoritativeDestruction();

	CleanupDeletedEntity(EntityId);
}

void USpatialReceiver::CleanupDeletedEntity(Worker_EntityId EntityId)
{
	NetDriver->GetEntityRegistry()->RemoveFromRegistry(EntityId);
	Cast<USpatialPackageMapClient>(NetDriver->GetSpatialOSNetConnection()->PackageMap)->RemoveEntityActor(EntityId);
}

UClass* USpatialReceiver::GetNativeEntityClass(Metadata* MetadataComponent)
{
	return FindObject<UClass>(ANY_PACKAGE, *MetadataComponent->EntityType);
}

// Note that in SpatialGDK, this function will not be called on the spawning worker.
// It's only for client, and in the future, other workers.
AActor* USpatialReceiver::SpawnNewEntity(Position* PositionComponent, UClass* ActorClass, bool bDeferred)
{
	FVector InitialLocation = Coordinates::ToFVector(PositionComponent->Coords);
	AActor* NewActor = nullptr;
	if (ActorClass)
	{
		//bRemoteOwned needs to be public in source code. This might be a controversial change.
		FActorSpawnParameters SpawnInfo;
		SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnInfo.bRemoteOwned = !NetDriver->IsServer();
		SpawnInfo.bNoFail = true;
		// We defer the construction in the GDK pipeline to allow initialization of replicated properties first.
		SpawnInfo.bDeferConstruction = bDeferred;

		FVector SpawnLocation = FRepMovement::RebaseOntoLocalOrigin(InitialLocation, World->OriginLocation);

		NewActor = World->SpawnActorAbsolute(ActorClass, FTransform(FRotator::ZeroRotator, SpawnLocation), SpawnInfo);
		check(NewActor);
	}

	return NewActor;
}

void USpatialReceiver::ApplyComponentData(Worker_EntityId EntityId, Worker_ComponentData& Data, USpatialActorChannel* Channel)
{
	UClass* Class= TypebindingManager->FindClassByComponentId(Data.component_id);
	checkf(Class, TEXT("Component %d isn't hand-written and not present in ComponentToClassMap."));

	UObject* TargetObject = GetTargetObjectFromChannelAndClass(Channel, Class);
	FChannelObjectPair ChannelObjectPair(Channel, TargetObject);

	FClassInfo* Info = TypebindingManager->FindClassInfoByClass(Class);
	check(Info);

	bool bAutonomousProxy = NetDriver->GetNetMode() == NM_Client && View->GetAuthority(EntityId, Info->RPCComponents[RPC_Client] == WORKER_AUTHORITY_AUTHORITATIVE);

	if (Data.component_id == Info->SingleClientComponent || Data.component_id == Info->MultiClientComponent)
	{
		FObjectReferencesMap& ObjectReferencesMap = UnresolvedRefsMap.FindOrAdd(ChannelObjectPair);
		TSet<UnrealObjectRef> UnresolvedRefs;

		ComponentReader Reader(NetDriver, ObjectReferencesMap, UnresolvedRefs);
		Reader.ApplyComponentData(Data, TargetObject, Channel);

		QueueIncomingRepUpdates(ChannelObjectPair, ObjectReferencesMap, UnresolvedRefs);
	}
	else if (Data.component_id == Info->HandoverComponent)
	{
		// TODO: Handover
	}
	else
	{
		UE_LOG(LogTemp, Verbose, TEXT("Skipping because RPC components don't have actual data."));
	}
}

void USpatialReceiver::OnComponentUpdate(Worker_ComponentUpdateOp& Op)
{
	if (View->GetAuthority(Op.entity_id, Op.update.component_id) == WORKER_AUTHORITY_AUTHORITATIVE)
	{
		UE_LOG(LogTemp, Verbose, TEXT("!!! Skipping because we sent this update"));
		return;
	}

	switch (Op.update.component_id)
	{
	case ENTITY_ACL_COMPONENT_ID:
	case METADATA_COMPONENT_ID:
	case POSITION_COMPONENT_ID:
	case PERSISTENCE_COMPONENT_ID:
	case SpatialConstants::PLAYER_SPAWNER_COMPONENT_ID:
	case UNREAL_METADATA_COMPONENT_ID:
		UE_LOG(LogTemp, Verbose, TEXT("!!! Skipping because this is hand-written Spatial component"));
		return;
	}

	UClass* Class = TypebindingManager->FindClassByComponentId(Op.update.component_id);
	checkf(Class, TEXT("Component %d isn't hand-written and not present in ComponentToClassMap."));
	FClassInfo* Info = TypebindingManager->FindClassInfoByClass(Class);
	check(Info);

	USpatialActorChannel* ActorChannel = NetDriver->GetActorChannelByEntityId(Op.entity_id);
	bool bIsServer = NetDriver->IsServer();

	if (Op.update.component_id == Info->SingleClientComponent)
	{
		check(ActorChannel);

		UObject* TargetObject = GetTargetObjectFromChannelAndClass(ActorChannel, Class);
		ApplyComponentUpdate(Op.update, TargetObject, ActorChannel);
	}
	else if (Op.update.component_id == Info->MultiClientComponent)
	{
		check(ActorChannel);

		UObject* TargetObject = GetTargetObjectFromChannelAndClass(ActorChannel, Class);
		ApplyComponentUpdate(Op.update, TargetObject, ActorChannel);
	}
	else if (Op.update.component_id == Info->HandoverComponent)
	{
		if (!bIsServer)
		{
			UE_LOG(LogTemp, Verbose, TEXT("!!! Skipping Handover component because we're a client."));
			return;
		}
		// TODO: Handover
	}
	else if (Op.update.component_id == Info->RPCComponents[RPC_NetMulticast])
	{
		check(ActorChannel);
		const TArray<UFunction*>& RPCArray = Info->RPCs.FindChecked(RPC_NetMulticast);
		ReceiveMulticastUpdate(Op.update, Op.entity_id, RPCArray);
	}
	else
	{
		UE_LOG(LogTemp, Verbose, TEXT("Skipping because it's an empty component update from an RPC component. (most likely as a result of gaining authority)"));
	}
}

void USpatialReceiver::OnCommandRequest(Worker_CommandRequestOp& Op)
{
	Schema_FieldId CommandIndex = Schema_GetCommandRequestCommandIndex(Op.request.schema_type);
	UE_LOG(LogTemp, Verbose, TEXT("Received command request (entity: %lld, component: %d, command: %d)"), Op.entity_id, Op.request.component_id, CommandIndex);

	if (Op.request.component_id == SpatialConstants::PLAYER_SPAWNER_COMPONENT_ID && CommandIndex == 1)
	{
		Schema_Object* Payload = Schema_GetCommandRequestObject(Op.request.schema_type);
		FString URLString = Schema_GetString(Payload, 1);

		URLString.Append(TEXT("?workerId=")).Append(UTF8_TO_TCHAR(Op.caller_worker_id));

		NetDriver->AcceptNewPlayer(FURL(nullptr, *URLString, TRAVEL_Absolute), false);

		Worker_CommandResponse CommandResponse = {};
		CommandResponse.component_id = SpatialConstants::PLAYER_SPAWNER_COMPONENT_ID;
		CommandResponse.schema_type = Schema_CreateCommandResponse(SpatialConstants::PLAYER_SPAWNER_COMPONENT_ID, 1);
		Schema_Object* ResponseObject = Schema_GetCommandResponseObject(CommandResponse.schema_type);
		Schema_AddBool(ResponseObject, 1, true);

		Worker_Connection_SendCommandResponse(NetDriver->Connection, Op.request_id, &CommandResponse);

		// TODO: Sahil - Eventually uncomment this but for now leave this block as is.
		//NetDriver->PlayerSpawner->ReceivePlayerSpawnRequest(Message, Op.caller_worker_id, Op.request_id);

		return;
	}

	Worker_CommandResponse Response = {};
	Response.component_id = Op.request.component_id;
	Response.schema_type = Schema_CreateCommandResponse(Op.request.component_id, CommandIndex);

	if (UClass* Class = TypebindingManager->FindClassByComponentId(Op.request.component_id))
	{
		FClassInfo* Info = TypebindingManager->FindClassInfoByClass(Class);
		check(Info);

		ERPCType RPCType = RPC_Count;
		for (int i = RPC_Client; i <= RPC_CrossServer; i++)
		{
			if (Info->RPCComponents[i] == Op.request.component_id)
			{
				RPCType = (ERPCType)i;
				break;
			}
		}
		check(RPCType <= RPC_CrossServer);

		const TArray<UFunction*>* RPCArray = Info->RPCs.Find(RPCType);
		check(RPCArray);
		check((int)CommandIndex - 1 < RPCArray->Num());

		UFunction* Function = (*RPCArray)[CommandIndex - 1];

		uint8* Parms = (uint8*)FMemory_Alloca(Function->ParmsSize);
		FMemory::Memzero(Parms, Function->ParmsSize);

		UObject* TargetObject = nullptr;
		ReceiveRPCCommandRequest(Op.request, Op.entity_id, Function, TargetObject, Parms);

		if (TargetObject)
		{
			TargetObject->ProcessEvent(Function, Parms);
		}

		// Destroy the parameters.
		for (TFieldIterator<UProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Parms);
		}
	}

	Sender->SendCommandResponse(Op.request_id, Response);
}

void USpatialReceiver::ApplyComponentUpdate(const Worker_ComponentUpdate& ComponentUpdate, UObject* TargetObject, USpatialActorChannel* Channel)
{
	FChannelObjectPair ChannelObjectPair(Channel, TargetObject);

	FObjectReferencesMap& ObjectReferencesMap = UnresolvedRefsMap.FindOrAdd(ChannelObjectPair);
	TSet<UnrealObjectRef> UnresolvedRefs;
	ComponentReader Reader(NetDriver, ObjectReferencesMap, UnresolvedRefs);
	Reader.ApplyComponentUpdate(ComponentUpdate, TargetObject, Channel);

	QueueIncomingRepUpdates(ChannelObjectPair, ObjectReferencesMap, UnresolvedRefs);
}

void USpatialReceiver::ReceiveMulticastUpdate(const Worker_ComponentUpdate& ComponentUpdate, Worker_EntityId EntityId, const TArray<UFunction*>& RPCArray)
{
	Schema_Object* EventsObject = Schema_GetComponentUpdateEvents(ComponentUpdate.schema_type);

	for (Schema_FieldId EventIndex = 1; (int)EventIndex <= RPCArray.Num(); EventIndex++)
	{
		UFunction* Function = RPCArray[EventIndex - 1];
		for (uint32 i = 0; i < Schema_GetObjectCount(EventsObject, EventIndex); i++)
		{
			uint8* Parms = (uint8*)FMemory_Alloca(Function->ParmsSize);
			FMemory::Memzero(Parms, Function->ParmsSize);

			Schema_Object* EventData = Schema_IndexObject(EventsObject, EventIndex, i);

			UnrealObjectRef TargetObjectRef;
			TargetObjectRef.Entity = EntityId;
			TargetObjectRef.Offset = Schema_GetUint32(EventData, 1);

			FNetworkGUID TargetNetGUID = PackageMap->GetNetGUIDFromUnrealObjectRef(TargetObjectRef);
			if (!TargetNetGUID.IsValid())
			{
				// TODO: Handle RPC to unresolved object
				checkNoEntry();
			}

			UObject* TargetObject = PackageMap->GetObjectFromNetGUID(TargetNetGUID, false);
			checkf(TargetObject, TEXT("Object Ref %s (NetGUID %s) does not correspond to a UObject."), *TargetObjectRef.ToString(), *TargetNetGUID.ToString());

			TSet<UnrealObjectRef> UnresolvedRefs;

			// TODO: Valentyn can you check this
			FString PayloadData = Schema_GetString(EventData, 2);
			// A bit hacky, we should probably include the number of bits with the data instead.
			int64 CountBits = PayloadData.Len() * 8;
			FSpatialNetBitReader PayloadReader(PackageMap, (uint8*)*PayloadData, CountBits, UnresolvedRefs);

			TSharedPtr<FRepLayout> RepLayout = NetDriver->GetFunctionRepLayout(Function);
			RepLayout_ReceivePropertiesForRPC(*RepLayout, PayloadReader, Parms);

			// TODO: Check for unresolved objects in the payload

			TargetObject->ProcessEvent(Function, Parms);

			// Destroy the parameters.
			// warning: highly dependent on UObject::ProcessEvent freeing of parms!
			for (TFieldIterator<UProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->DestroyValue_InContainer(Parms);
			}
		}
	}
}

UObject* USpatialReceiver::GetTargetObjectFromChannelAndClass(USpatialActorChannel* Channel, UClass* Class)
{
	UObject* TargetObject = nullptr;

	if (Class->IsChildOf<AActor>())
	{
		check(Channel->Actor->IsA(Class));
		TargetObject = Channel->Actor;
	}
	else if (Class->IsChildOf<UActorComponent>())
	{
		FClassInfo* ActorInfo = TypebindingManager->FindClassInfoByClass(Channel->Actor->GetClass());
		check(ActorInfo);
		check(ActorInfo->ComponentClasses.Find(Class));
		TArray<UActorComponent*> Components = Channel->Actor->GetComponentsByClass(Class);
		checkf(Components.Num() == 1, TEXT("Multiple replicated components of the same type are currently not supported by Unreal GDK"));
		TargetObject = Components[0];
	}
	else
	{
		checkNoEntry();
	}

	check(TargetObject);
	return TargetObject;
}

void USpatialReceiver::OnReserveEntityIdResponse(Worker_ReserveEntityIdResponseOp& Op)
{
	if (USpatialActorChannel* Channel = PopPendingActorRequest(Op.request_id))
	{
		Channel->OnReserveEntityIdResponse(Op);
	}
}

void USpatialReceiver::OnCreateEntityIdResponse(Worker_CreateEntityResponseOp& Op)
{
	if (USpatialActorChannel* Channel = PopPendingActorRequest(Op.request_id))
	{
		Channel->OnCreateEntityResponse(Op);
	}
}

void USpatialReceiver::AddPendingActorRequest(Worker_RequestId RequestId)
{
	PendingActorRequests.Add(RequestId);
}

USpatialActorChannel* USpatialReceiver::PopPendingActorRequest(Worker_RequestId RequestId)
{
	USpatialActorChannel** Channel = PendingActorRequests.Find(RequestId);
	if (Channel == nullptr)
	{
		return nullptr;
	}
	PendingActorRequests.Remove(RequestId);
	return *Channel;
}

void USpatialReceiver::ProcessQueuedResolvedObjects()
{
	for (TPair<UObject*, UnrealObjectRef>& It : ResolvedObjectQueue)
	{
		ResolvePendingOperations_Internal(It.Key, It.Value);
	}
	ResolvedObjectQueue.Empty();
}

void USpatialReceiver::ResolvePendingOperations(UObject* Object, const UnrealObjectRef& ObjectRef)
{
	if (bInCriticalSection)
	{
		ResolvedObjectQueue.Add(TPair<UObject*, UnrealObjectRef>{ Object, ObjectRef });
	}
	else
	{
		ResolvePendingOperations_Internal(Object, ObjectRef);
	}
}

void USpatialReceiver::QueueIncomingRepUpdates(FChannelObjectPair ChannelObjectPair, const FObjectReferencesMap& ObjectReferencesMap, const TSet<UnrealObjectRef>& UnresolvedRefs)
{
	for (const UnrealObjectRef& UnresolvedRef : UnresolvedRefs)
	{
		UE_LOG(LogTemp, Log, TEXT("Added pending incoming property for object ref: %s, target object: %s"), *UnresolvedRef.ToString(), *ChannelObjectPair.Value->GetName());
		IncomingRefsMap.FindOrAdd(UnresolvedRef).Add(ChannelObjectPair);
	}

	if (ObjectReferencesMap.Num() == 0)
	{
		UnresolvedRefsMap.Remove(ChannelObjectPair);
	}
}

void USpatialReceiver::ResolvePendingOperations_Internal(UObject* Object, const UnrealObjectRef& ObjectRef)
{
	UE_LOG(LogTemp, Log, TEXT("!!! Resolving pending object refs and RPCs which depend on object: %s %s."), *Object->GetName(), *ObjectRef.ToString());
	Sender->ResolveOutgoingOperations(Object);
	ResolveIncomingOperations(Object, ObjectRef);
	Sender->ResolveOutgoingRPCs(Object);
}

void USpatialReceiver::ResolveIncomingOperations(UObject* Object, const UnrealObjectRef& ObjectRef)
{
	// TODO: queue up resolved objects since they were resolved during process ops
	// and then resolve all of them at the end of process ops

	TSet<FChannelObjectPair>* TargetObjectSet = IncomingRefsMap.Find(ObjectRef);
	if (!TargetObjectSet) return;

	UE_LOG(LogTemp, Log, TEXT("!!! Resolving incoming operations depending on object ref %s, resolved object: %s"), *ObjectRef.ToString(), *Object->GetName());

	for (FChannelObjectPair& ChannelObjectPair : *TargetObjectSet)
	{
		FObjectReferencesMap* UnresolvedRefs = UnresolvedRefsMap.Find(ChannelObjectPair);

		if (!UnresolvedRefs)
		{
			continue;
		}

		USpatialActorChannel* DependentChannel = ChannelObjectPair.Key;
		UObject* ReplicatingObject = ChannelObjectPair.Value;

		bool bStillHasUnresolved = false;
		bool bSomeObjectsWereMapped = false;
		TArray<UProperty*> RepNotifies;

		FRepLayout& RepLayout = DependentChannel->GetObjectRepLayout(ReplicatingObject);
		FRepStateStaticBuffer& ShadowData = DependentChannel->GetObjectStaticBuffer(ReplicatingObject);

		ResolveObjectReferences(RepLayout, ReplicatingObject, *UnresolvedRefs, ShadowData.GetData(), (uint8*)ReplicatingObject, ShadowData.Num(), RepNotifies, bSomeObjectsWereMapped, bStillHasUnresolved);

		if (bSomeObjectsWereMapped)
		{
			UE_LOG(LogTemp, Log, TEXT("!!! Resolved for target object %s"), *ReplicatingObject->GetName());
			DependentChannel->PostReceiveSpatialUpdate(ReplicatingObject, RepNotifies);
		}

		if (!bStillHasUnresolved)
		{
			UnresolvedRefsMap.Remove(ChannelObjectPair);
		}
	}

	IncomingRefsMap.Remove(ObjectRef);
}

void USpatialReceiver::ResolveObjectReferences(FRepLayout& RepLayout, UObject* ReplicatedObject, FObjectReferencesMap& ObjectReferencesMap, uint8* RESTRICT StoredData, uint8* RESTRICT Data, int32 MaxAbsOffset, TArray<UProperty*>& RepNotifies, bool& bOutSomeObjectsWereMapped, bool& bOutStillHasUnresolved)
{
	for (auto It = ObjectReferencesMap.CreateIterator(); It; ++It)
	{
		int32 AbsOffset = It.Key();

		if (AbsOffset >= MaxAbsOffset)
		{
			UE_LOG(LogTemp, Log, TEXT("!!! ResolveObjectReferences: Removed unresolved reference: AbsOffset >= MaxAbsOffset: %d"), AbsOffset);
			It.RemoveCurrent();
			continue;
		}

		FObjectReferences& ObjectReferences = It.Value();
		UProperty* Property = ObjectReferences.Property;
		FRepParentCmd& Parent = RepLayout.Parents[ObjectReferences.ParentIndex];

		if (ObjectReferences.Array)
		{
			check(Property->IsA<UArrayProperty>());

			// TODO: storedarray's data will be invalidated if this is the first resolved ref 
			Property->CopySingleValue(StoredData + AbsOffset, Data + AbsOffset);

			FScriptArray* StoredArray = (FScriptArray*)(StoredData + AbsOffset);
			FScriptArray* Array = (FScriptArray*)(Data + AbsOffset);

			int32 NewMaxOffset = FMath::Min(StoredArray->Num(), Array->Num()) * Property->ElementSize;

			bool bArrayHasUnresolved = false;
			ResolveObjectReferences(RepLayout, ReplicatedObject, *ObjectReferences.Array, (uint8*)StoredArray->GetData(), (uint8*)Array->GetData(), NewMaxOffset, RepNotifies, bOutSomeObjectsWereMapped, bArrayHasUnresolved);
			if (!bArrayHasUnresolved)
			{
				It.RemoveCurrent();
			}
			else
			{
				bOutStillHasUnresolved = true;
			}
			continue;
		}

		bool bResolvedSomeRefs = false;
		UObject* SinglePropObject = nullptr;

		for (auto UnresolvedIt = ObjectReferences.UnresolvedRefs.CreateIterator(); UnresolvedIt; ++UnresolvedIt)
		{
			UnrealObjectRef& ObjectRef = *UnresolvedIt;

			FNetworkGUID NetGUID = PackageMap->GetNetGUIDFromUnrealObjectRef(ObjectRef);
			if (NetGUID.IsValid())
			{
				UObject* Object = PackageMap->GetObjectFromNetGUID(NetGUID, true);
				check(Object);

				UE_LOG(LogTemp, Log, TEXT("!!! ResolveObjectReferences: Resolved object ref: Offset: %d, Object ref: %s, PropName: %s, ObjName: %s"), AbsOffset, *ObjectRef.ToString(), *Property->GetNameCPP(), *Object->GetName());

				UnresolvedIt.RemoveCurrent();
				bResolvedSomeRefs = true;

				if (ObjectReferences.bSingleProp)
				{
					SinglePropObject = Object;
				}
			}
		}

		if (bResolvedSomeRefs)
		{
			if (!bOutSomeObjectsWereMapped)
			{
				ReplicatedObject->PreNetReceive();
				bOutSomeObjectsWereMapped = true;
			}

			//if (Parent.Property->HasAnyPropertyFlags(CPF_RepNotify))
			//{
			//	Property->CopySingleValue(StoredData + AbsOffset, Data + AbsOffset);
			//}

			if (ObjectReferences.bSingleProp)
			{
				UObjectPropertyBase* ObjectProperty = Cast<UObjectPropertyBase>(Property);
				check(ObjectProperty);

				ObjectProperty->SetObjectPropertyValue(Data + AbsOffset, SinglePropObject);
			}
			else
			{
				// TODO: Valentyn can you fix this
				//FSpatialNetBitReader Reader(PackageMap, ObjectReferences.Buffer.GetData(), ObjectReferences.NumBufferBits);
				//check(Property->IsA<UStructProperty>());
				//ReadStructProperty(Reader, PackageMap, Cast<UStructProperty>(Property), Data + AbsOffset, Driver, bOutStillHasUnresolved);
			}

			if (Parent.Property->HasAnyPropertyFlags(CPF_RepNotify))
			{
				if (Parent.RepNotifyCondition == REPNOTIFY_Always || !Property->Identical(StoredData + AbsOffset, Data + AbsOffset))
				{
					RepNotifies.AddUnique(Parent.Property);
				}
			}
		}

		if (ObjectReferences.UnresolvedRefs.Num() > 0)
		{
			bOutStillHasUnresolved = true;
		}
		else
		{
			It.RemoveCurrent();
		}
	}
}

void USpatialReceiver::ReceiveRPCCommandRequest(const Worker_CommandRequest& CommandRequest, Worker_EntityId EntityId, UFunction* Function, UObject*& OutTargetObject, void* Data)
{
	// TODO: Valentyn check this function
	Schema_Object* RequestObject = Schema_GetCommandRequestObject(CommandRequest.schema_type);

	UnrealObjectRef TargetObjectRef;
	TargetObjectRef.Entity = EntityId;
	TargetObjectRef.Offset = Schema_GetUint32(RequestObject, 1);

	FNetworkGUID TargetNetGUID = PackageMap->GetNetGUIDFromUnrealObjectRef(TargetObjectRef);
	if (!TargetNetGUID.IsValid())
	{
		// TODO: Handle RPC to unresolved object
		checkNoEntry();
	}

	OutTargetObject = PackageMap->GetObjectFromNetGUID(TargetNetGUID, false);
	checkf(OutTargetObject, TEXT("Object Ref %s (NetGUID %s) does not correspond to a UObject."), *TargetObjectRef.ToString(), *TargetNetGUID.ToString());

	TSet<UnrealObjectRef> UnresolvedRefs;

	FString PayloadData = Schema_GetString(RequestObject, 2);
	// A bit hacky, we should probably include the number of bits with the data instead.
	int64 CountBits = PayloadData.Len() * 8;
	FSpatialNetBitReader PayloadReader(PackageMap, (uint8*)*PayloadData, CountBits, UnresolvedRefs);

	TSharedPtr<FRepLayout> RepLayout = NetDriver->GetFunctionRepLayout(Function);
	RepLayout_ReceivePropertiesForRPC(*RepLayout, PayloadReader, Data);

	// TODO: Check for unresolved objects in the payload
}