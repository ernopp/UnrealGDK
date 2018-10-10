// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"

#include "Utils/SchemaUtils.h"

#include "EngineClasses/SpatialNetDriver.h" // TODO: Remove this.

#include <improbable/c_schema.h>
#include <improbable/c_worker.h>

#include "GlobalStateManager.generated.h"

class USpatialNetDriver;
class USpatialActorChannel;
class USpatialStaticComponentView;
class USpatialSender;

DECLARE_LOG_CATEGORY_EXTERN(LogGlobalStateManager, Log, All)

DECLARE_DELEGATE_OneParam(AcceptingPlayersDelegate, bool);

UCLASS()
class SPATIALGDK_API UGlobalStateManager : public UObject
{
	GENERATED_BODY()

public:

	void Init(USpatialNetDriver* InNetDriver, FTimerManager* InTimerManager);

	void ApplyData(const Worker_ComponentData& Data);
	void ApplyMapData(const Worker_ComponentData& Data);
	void ApplyUpdate(const Worker_ComponentUpdate& Update);
	void ApplyMapUpdate(const Worker_ComponentUpdate& Update);
	void LinkExistingSingletonActors();
	void ExecuteInitialSingletonActorReplication();
	void UpdateSingletonEntityId(const FString& ClassName, const Worker_EntityId SingletonEntityId);

	bool IsSingletonEntity(Worker_EntityId EntityId);

	void QueryGSM();
	void RetryQueryGSM();
	void SetDeploymentMapURL(FString MapURL);
	void WorldWipe(const USpatialNetDriver::ServerTravelDelegate& Delegate);
	void DeleteEntities(const Worker_EntityQueryResponseOp& Op);
	void LoadSnapshot(FString SnapshotName);
	void ToggleAcceptingPlayers(bool bAcceptingPlayers);

	FString DeploymentMapURL;
	bool bAcceptingPlayers = false;
	bool bHasLiveMapAuthority = false;

	AcceptingPlayersDelegate AcceptingPlayersChanged;

private:
	void GetSingletonActorAndChannel(FString ClassName, AActor*& OutActor, USpatialActorChannel*& OutChannel);

private:
	UPROPERTY()
	USpatialNetDriver* NetDriver;

	UPROPERTY()
	USpatialStaticComponentView* StaticComponentView;

	UPROPERTY()
	USpatialSender* Sender;

	UPROPERTY()
	USpatialReceiver* Receiver;

	StringToEntityMap SingletonNameToEntityId;

	Worker_EntityId GlobalStateManagerEntityId;

	FTimerManager* TimerManager;
};
