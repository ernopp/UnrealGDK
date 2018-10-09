// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "SpatialStaticComponentView.h"


Worker_Authority USpatialStaticComponentView::GetAuthority(Worker_EntityId EntityId, Worker_ComponentId ComponentId)
{
	if (TMap<Worker_ComponentId, Worker_Authority>* ComponentAuthorityMap = EntityComponentAuthorityMap.Find(EntityId))
	{
		if (Worker_Authority* Authority = ComponentAuthorityMap->Find(ComponentId))
		{
			return *Authority;
		}
	}

	return WORKER_AUTHORITY_NOT_AUTHORITATIVE;
}

template <typename T>
typename T* USpatialStaticComponentView::GetComponentData(Worker_EntityId EntityId)
{
	if (TMap<Worker_ComponentId, TUniquePtr<ComponentStorageBase>>* ComponentStorageMap = EntityComponentMap.Find(EntityId))
	{
		if (TUniquePtr<improbable::ComponentStorageBase>* Component = ComponentStorageMap->Find(T::ComponentId))
		{
			return &(static_cast<improbable::ComponentStorage<T>*>(Component->Get())->Get());
		}
	}

	return nullptr;
}

void USpatialStaticComponentView::OnAddComponent(const Worker_AddComponentOp& Op)
{
	TUniquePtr<improbable::ComponentStorageBase> Data;
	switch (Op.data.component_id)
	{
	case SpatialConstants::ENTITY_ACL_COMPONENT_ID:
		Data = MakeUnique<improbable::ComponentStorage<improbable::EntityAcl>>(Op.data);
		break;
	case SpatialConstants::METADATA_COMPONENT_ID:
		Data = MakeUnique<improbable::ComponentStorage<improbable::Metadata>>(Op.data);
		break;
	case SpatialConstants::POSITION_COMPONENT_ID:
		Data = MakeUnique<improbable::ComponentStorage<improbable::Position>>(Op.data);
		break;
	case SpatialConstants::PERSISTENCE_COMPONENT_ID:
		Data = MakeUnique<improbable::ComponentStorage<improbable::Persistence>>(Op.data);
		break;
	case SpatialConstants::ROTATION_COMPONENT_ID:
		Data = MakeUnique<improbable::ComponentStorage<improbable::Rotation>>(Op.data);
		break;
	case SpatialConstants::UNREAL_METADATA_COMPONENT_ID:
		Data = MakeUnique<improbable::ComponentStorage<improbable::UnrealMetadata>>(Op.data);
		break;
	default:
		return;
	}

	EntityComponentMap.FindOrAdd(Op.entity_id).FindOrAdd(Op.data.component_id) = std::move(Data);
}

void USpatialStaticComponentView::OnRemoveEntity(const Worker_RemoveEntityOp& Op)
{
	EntityComponentMap.Remove(Op.entity_id);
}

void USpatialStaticComponentView::OnComponentUpdate(const Worker_ComponentUpdateOp& Op)
{
	switch (Op.update.component_id)
	{
	case SpatialConstants::ENTITY_ACL_COMPONENT_ID:
		if (improbable::EntityAcl* aclComponent = GetComponentData<improbable::EntityAcl>(Op.entity_id))
		{
			aclComponent->ApplyComponentUpdate(Op.update);
		}
		break;
	case SpatialConstants::POSITION_COMPONENT_ID:
		if (improbable::Position* positionComponent = GetComponentData<improbable::Position>(Op.entity_id))
		{
			positionComponent->ApplyComponentUpdate(Op.update);
		}
		break;
	case SpatialConstants::ROTATION_COMPONENT_ID:
		if (improbable::Rotation* rotationComponent = GetComponentData<improbable::Rotation>(Op.entity_id))
		{
			rotationComponent->ApplyComponentUpdate(Op.update);
		}
		break;
	default:
		return;
	}
}

void USpatialStaticComponentView::OnAuthorityChange(const Worker_AuthorityChangeOp& Op)
{
	EntityComponentAuthorityMap.FindOrAdd(Op.entity_id).FindOrAdd(Op.component_id) = (Worker_Authority)Op.authority;
}