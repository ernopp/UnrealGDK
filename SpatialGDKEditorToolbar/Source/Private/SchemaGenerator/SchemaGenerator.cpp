// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "SchemaGenerator.h"

#include "Algo/Reverse.h"

#include "Utils/CodeWriter.h"
#include "Utils/ComponentIdGenerator.h"
#include "Utils/DataTypeUtilities.h"
#include "SpatialTypebindingManager.h"

// Given a RepLayout cmd type (a data type supported by the replication system). Generates the corresponding
// type used in schema.
FString PropertyToSchemaType(UProperty* Property, bool bIsRPCProperty)
{
	FString DataType;

	// For RPC arguments we may wish to handle them differently.
	if (bIsRPCProperty)
	{
		if (Property->ArrayDim > 1) // Static arrays in RPC arguments are replicated as lists.
		{
			DataType = PropertyToSchemaType(Property, false); // Have to get the type of the property inside the static array.
			DataType = FString::Printf(TEXT("list<%s>"), *DataType);
			return DataType;
		}
	}

	if (Property->IsA(UStructProperty::StaticClass()))
	{
		UStructProperty* StructProp = Cast<UStructProperty>(Property);
		UScriptStruct* Struct = StructProp->Struct;
		if (Struct->StructFlags & STRUCT_NetSerializeNative)
		{
			// Specifically when NetSerialize is implemented for a struct we want to use 'bytes'.
			// This includes RepMovement and UniqueNetId.
			DataType = TEXT("bytes");
		}
		else
		{
			DataType = TEXT("bytes");
		}
	}
	else if (Property->IsA(UBoolProperty::StaticClass()))
	{
		DataType = TEXT("bool");
	}
	else if (Property->IsA(UFloatProperty::StaticClass()))
	{
		DataType = TEXT("float");
	}
	else if (Property->IsA(UDoubleProperty::StaticClass()))
	{
		DataType = TEXT("double");
	}
	else if (Property->IsA(UInt8Property::StaticClass()))
	{
		DataType = TEXT("int32");
	}
	else if (Property->IsA(UInt16Property::StaticClass()))
	{
		DataType = TEXT("int32");
	}
	else if (Property->IsA(UIntProperty::StaticClass()))
	{
		DataType = TEXT("int32");
	}
	else if (Property->IsA(UInt64Property::StaticClass()))
	{
		DataType = TEXT("int64");
	}
	else if (Property->IsA(UByteProperty::StaticClass()))
	{
		DataType = TEXT("uint32"); // uint8 not supported in schema.
	}
	else if (Property->IsA(UUInt16Property::StaticClass()))
	{
		DataType = TEXT("uint32");
	}
	else if (Property->IsA(UUInt32Property::StaticClass()))
	{
		DataType = TEXT("uint32");
	}
	else if (Property->IsA(UUInt64Property::StaticClass()))
	{
		DataType = TEXT("uint64");
	}
	else if (Property->IsA(UNameProperty::StaticClass()) || Property->IsA(UStrProperty::StaticClass()) || Property->IsA(UTextProperty::StaticClass()))
	{
		DataType = TEXT("string");
	}
	else if (Property->IsA(UObjectPropertyBase::StaticClass()))
	{
		DataType = TEXT("UnrealObjectRef");
	}
	else if (Property->IsA(UArrayProperty::StaticClass()))
	{
		DataType = PropertyToSchemaType(Cast<UArrayProperty>(Property)->Inner, bIsRPCProperty);
		DataType = FString::Printf(TEXT("list<%s>"), *DataType);
	}
	else if (Property->IsA(UEnumProperty::StaticClass()))
	{
		DataType = GetEnumDataType(Cast<UEnumProperty>(Property));
	}
	else
	{
		DataType = TEXT("bytes");
	}

	return DataType;
}

void WriteSchemaRepField(FCodeWriter& Writer, const TSharedPtr<FUnrealProperty> RepProp, const FString& PropertyPath, const int FieldCounter)
{
	Writer.Printf("%s %s = %d;",
		*PropertyToSchemaType(RepProp->Property, false),
		*SchemaFieldName(RepProp),
		FieldCounter
	);
}

void WriteSchemaHandoverField(FCodeWriter& Writer, const TSharedPtr<FUnrealProperty> HandoverProp, const int FieldCounter)
{
	Writer.Printf("%s %s = %d;",
		*PropertyToSchemaType(HandoverProp->Property, false),
		*SchemaFieldName(HandoverProp),
		FieldCounter
	);
}

void WriteSchemaRPCField(TSharedPtr<FCodeWriter> Writer, const TSharedPtr<FUnrealProperty> RPCProp, const int FieldCounter)
{
	Writer->Printf("%s %s = %d;",
		*PropertyToSchemaType(RPCProp->Property, true),
		*SchemaFieldName(RPCProp),
		FieldCounter
	);
}

// core_types.schema should only be included if any components in the file have
// 1. An UnrealObjectRef
// 2. A list of UnrealObjectRefs
// 3. An RPC
bool ShouldIncludeCoreTypes(TSharedPtr<FUnrealType>& TypeInfo)
{
	FUnrealFlatRepData RepData = GetFlatRepData(TypeInfo);

	for (auto& PropertyGroup : RepData)
	{
		for (auto& PropertyPair : PropertyGroup.Value)
		{
			UProperty* Property = PropertyPair.Value->Property;
			if(Property->IsA<UObjectPropertyBase>())
			{
				return true;
			}

			if (Property->IsA<UArrayProperty>())
			{
				if (Cast<UArrayProperty>(Property)->Inner->IsA<UObjectPropertyBase>())
				{
					return true;
				}
			}
		}
	}

	if (TypeInfo->RPCs.Num() > 0)
	{
		return true;
	}

	for (auto& PropertyPair : TypeInfo->Properties)
	{
		UProperty* Property = PropertyPair.Key;
		if (Property->IsA<UObjectPropertyBase>() && PropertyPair.Value->Type.IsValid())
		{
			if (PropertyPair.Value->Type->RPCs.Num() > 0)
			{
				return true;
			}
		}
	}

	return false;
}

bool IsReplicatedActorComponent(TSharedPtr<FUnrealType> TypeInfo)
{
	if (GetFlatRepData(TypeInfo)[REP_MultiClient].Num() > 0 || GetFlatRepData(TypeInfo)[REP_SingleClient].Num() > 0)
	{
		return true;
	}

	if (GetFlatHandoverData(TypeInfo).Num() > 0)
	{
		return true;
	}

	if (TypeInfo->RPCs.Num() > 0)
	{
		return true;
	}

	return false;
}

void GenerateActorComponentSchema(UClass* Class, TSharedPtr<FUnrealType> TypeInfo, FString SchemaPath)
{
	FCodeWriter Writer;

	Writer.Printf(R"""(
		// Copyright (c) Improbable Worlds Ltd, All Rights Reserved
		// Note that this file has been generated automatically
		package unreal.generated;)""");

	if (ShouldIncludeCoreTypes(TypeInfo))
	{
		Writer.PrintNewLine();
		Writer.Printf("import \"unreal/gdk/core_types.schema\";");
	}

	FUnrealFlatRepData RepData = GetFlatRepData(TypeInfo);

	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		if (RepData[Group].Num() == 0)
		{
			continue;
		}

		Writer.PrintNewLine();
		Writer.Printf("type %s {", *SchemaReplicatedDataName(Group, Class));
		Writer.Indent();
		for (auto& RepProp : RepData[Group])
		{
			WriteSchemaRepField(Writer,
				RepProp.Value,
				TEXT(""),
				RepProp.Value->ReplicationData->Handle);
		}
		Writer.Outdent().Print("}");
	}

	FCmdHandlePropertyMap HandoverData = GetFlatHandoverData(TypeInfo);
	if (HandoverData.Num() > 0)
	{
		Writer.PrintNewLine();

		Writer.Printf("type %s {", *SchemaHandoverDataName(Class));
		Writer.Indent();
		int FieldCounter = 0;
		for (auto& Prop : HandoverData)
		{
			FieldCounter++;
			WriteSchemaHandoverField(Writer,
				Prop.Value,
				FieldCounter);
		}
		Writer.Outdent().Print("}");
	}

	Writer.WriteToFile(FString::Printf(TEXT("%s%s.schema"), *SchemaPath, *UnrealNameToSchemaTypeName(Class->GetName())));
}


int GenerateActorSchema(int ComponentId, UClass* Class, TSharedPtr<FUnrealType> TypeInfo, FString SchemaPath)
{
	FCodeWriter Writer;

	FComponentIdGenerator IdGenerator(ComponentId);

	Writer.Printf(R"""(
		// Copyright (c) Improbable Worlds Ltd, All Rights Reserved
		// Note that this file has been generated automatically
		package unreal.generated.%s;)""",
		*UnrealNameToSchemaTypeName(Class->GetName().ToLower()));

	if (ShouldIncludeCoreTypes(TypeInfo))
	{
		Writer.PrintNewLine();
		Writer.Printf("import \"unreal/gdk/core_types.schema\";");
	}

	FSchemaData ActorSchemaData;
	ActorSchemaData.Class = Class;

	FUnrealFlatRepData RepData = GetFlatRepData(TypeInfo);

	// Client-server replicated properties.
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		if (RepData[Group].Num() == 0)
		{
			continue;
		}

		Writer.PrintNewLine();

		Writer.Printf("component %s {", *SchemaReplicatedDataName(Group, Class));
		Writer.Indent();
		Writer.Printf("id = %d;", IdGenerator.GetNextAvailableId());

		if (Group == REP_MultiClient)
		{
			ActorSchemaData.SchemaComponents[EComponentType::TYPE_Data] = IdGenerator.GetCurrentId();
		}
		else if (Group == REP_SingleClient)
		{
			ActorSchemaData.SchemaComponents[EComponentType::TYPE_OwnerOnly] = IdGenerator.GetCurrentId();
		}

		int FieldCounter = 0;
		for (auto& RepProp : RepData[Group])
		{
			FString ParentClassName = TEXT("");

			// This loop will add the owner class of each field in the component. Meant for short-term debugging only.
			// TODO UNR-166: Delete this when InteropCodegen is in a more complete state.
			FString PropertyPath;
			TSharedPtr<FUnrealProperty> UnrealProperty = RepProp.Value;
			while (UnrealProperty->ContainerType != nullptr)
			{
				TSharedPtr<FUnrealType> ContainerType = UnrealProperty->ContainerType.Pin();
				check(ContainerType.IsValid());
				if (ContainerType->ParentProperty != nullptr)
				{
					TSharedPtr<FUnrealProperty> ParentProperty = ContainerType->ParentProperty.Pin();
					if (ParentProperty.IsValid())
					{
						PropertyPath += FString::Printf(TEXT("%s::%s"), *ContainerType->Type->GetName(), *ParentProperty->Property->GetName());
					}
					UnrealProperty = ParentProperty;
				}
				else
				{
					break;
				}
			}

			if (UObject* ObjOuter = UnrealProperty->Property->GetOuter())
			{
				PropertyPath += FString::Printf(TEXT("::%s"), *ObjOuter->GetName());
			}

			FieldCounter++;
			WriteSchemaRepField(Writer,
				RepProp.Value,
				PropertyPath,
				RepProp.Value->ReplicationData->Handle);
		}
		Writer.Outdent().Print("}");
	}

	FCmdHandlePropertyMap HandoverData = GetFlatHandoverData(TypeInfo);
	if (HandoverData.Num() > 0)
	{
		Writer.PrintNewLine();

		// Handover (server to server) replicated properties.
		Writer.Printf("component %s {", *SchemaHandoverDataName(Class));
		Writer.Indent();
		Writer.Printf("id = %d;", IdGenerator.GetNextAvailableId());

		ActorSchemaData.SchemaComponents[EComponentType::TYPE_Handover] = IdGenerator.GetCurrentId();

		int FieldCounter = 0;
		for (auto& Prop : HandoverData)
		{
			FieldCounter++;
			WriteSchemaHandoverField(Writer,
				Prop.Value,
				FieldCounter);
		}
		Writer.Outdent().Print("}");
	}

	// RPC components.
	FUnrealRPCsByType RPCsByType = GetAllRPCsByType(TypeInfo);

	TArray<FString> ReliableMulticasts;

	for (auto Group : GetRPCTypes())
	{
		if (RPCsByType[Group].Num() == 0 && Group != RPC_Client)
		{
			continue;
		}

		Writer.PrintNewLine();

		Writer.Printf("component %s {", *SchemaRPCComponentName(Group, Class));
		Writer.Indent();
		Writer.Printf("id = %i;", IdGenerator.GetNextAvailableId());

		if (Group == RPC_Client)
		{
			ActorSchemaData.SchemaComponents[EComponentType::TYPE_ClientRPC] = IdGenerator.GetCurrentId();
		}
		else if (Group == RPC_Server)
		{
			ActorSchemaData.SchemaComponents[EComponentType::TYPE_ServerRPC] = IdGenerator.GetCurrentId();
		}
		else if (Group == RPC_NetMulticast)
		{
			ActorSchemaData.SchemaComponents[EComponentType::TYPE_NetMulticastRPC] = IdGenerator.GetCurrentId();
		}
		else if (Group == RPC_CrossServer)
		{
			ActorSchemaData.SchemaComponents[EComponentType::TYPE_CrossServerRPC] = IdGenerator.GetCurrentId();
		}

		for (auto& RPC : RPCsByType[Group])
		{
			if (Group == ERPCType::RPC_NetMulticast)
			{
				if (RPC->bReliable)
				{
					ReliableMulticasts.Add(FString::Printf(TEXT("%s::%s"), *GetFullCPPName(Class), *RPC->Function->GetName()));
				}

				Writer.Printf("event UnrealRPCCommandRequest %s;",
					*SchemaRPCName(Class, RPC->Function));
			}
			else
			{
				Writer.Printf("command UnrealRPCCommandResponse %s(UnrealRPCCommandRequest);",
					*SchemaRPCName(Class, RPC->Function));
			}
		}
		Writer.Outdent().Print("}");
	}

	GenerateActorComponentSchemaForActor(IdGenerator, Class, TypeInfo, SchemaPath, ActorSchemaData);

	if (ReliableMulticasts.Num() > 0)
	{
		FString AllReliableMulticasts;
		for (const FString& FunctionName : ReliableMulticasts)
		{
			AllReliableMulticasts += FunctionName + TEXT("\n");					
		}

		UE_LOG(LogTemp, Warning, TEXT("Unreal GDK currently does not support Reliable Multicast RPCs. These RPC will be treated as unreliable:\n%s"), *AllReliableMulticasts);
	}

	SchemaDatabase->ClassToSchema.Add(Class, ActorSchemaData);

	Writer.WriteToFile(FString::Printf(TEXT("%s%s.schema"), *SchemaPath, *UnrealNameToSchemaTypeName(Class->GetName())));

	return IdGenerator.GetNumUsedIds();
}

FSubobjectSchemaData GenerateActorComponentSpecificSchema(FCodeWriter& Writer, FComponentIdGenerator& IdGenerator, FString PropertyName, TSharedPtr<FUnrealType>& TypeInfo, UClass* ComponentClass)
{
	FUnrealFlatRepData RepData = GetFlatRepData(TypeInfo);

	FSubobjectSchemaData SubobjectData;
	SubobjectData.Class = ComponentClass;

	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		if (RepData[Group].Num() == 0)
		{
			continue;
		}

		Writer.PrintNewLine();

		FString ComponentName = PropertyName + GetReplicatedPropertyGroupName(Group);
		Writer.Printf("component %s {", *ComponentName);
		Writer.Indent();
		Writer.Printf("id = %d;", IdGenerator.GetNextAvailableId());
		Writer.Printf("data %s;", *SchemaReplicatedDataName(Group, ComponentClass));
		Writer.Outdent().Print("}");

		if (Group == REP_MultiClient)
		{
			SubobjectData.SchemaComponents[EComponentType::TYPE_Data] = IdGenerator.GetCurrentId();
		}
		else if (Group == REP_SingleClient)
		{
			SubobjectData.SchemaComponents[EComponentType::TYPE_OwnerOnly] = IdGenerator.GetCurrentId();
		}
	}

	FCmdHandlePropertyMap HandoverData = GetFlatHandoverData(TypeInfo);
	if (HandoverData.Num() > 0)
	{
		Writer.PrintNewLine();

		// Handover (server to server) replicated properties.
		Writer.Printf("component %s {", *(PropertyName + TEXT("Handover")));
		Writer.Indent();
		Writer.Printf("id = %d;", IdGenerator.GetNextAvailableId());
		Writer.Printf("data %s;", *SchemaHandoverDataName(ComponentClass));
		Writer.Outdent().Print("}");

		SubobjectData.SchemaComponents[EComponentType::TYPE_Handover] = IdGenerator.GetCurrentId();
	}

	FUnrealRPCsByType RPCsByType = GetAllRPCsByType(TypeInfo);

	for (auto Group : GetRPCTypes())
	{
		if (RPCsByType[Group].Num() == 0 && Group != RPC_Client)
		{
			continue;
		}

		Writer.PrintNewLine();

		FString ComponentName = PropertyName + GetRPCTypeName(Group) + TEXT("RPCs");
		Writer.Printf("component %s {", *ComponentName);
		Writer.Indent();
		Writer.Printf("id = %i;", IdGenerator.GetNextAvailableId());
		for (auto& RPC : RPCsByType[Group])
		{
			if (Group == ERPCType::RPC_NetMulticast)
			{
				Writer.Printf("event UnrealRPCCommandRequest %s;",
					*SchemaRPCName(Cast<UClass>(ComponentClass), RPC->Function));
			}
			else
			{
				Writer.Printf("command UnrealRPCCommandResponse %s(UnrealRPCCommandRequest);",
					*SchemaRPCName(Cast<UClass>(ComponentClass), RPC->Function));
			}
		}
		Writer.Outdent().Print("}");

		if (Group == RPC_Client)
		{
			SubobjectData.SchemaComponents[EComponentType::TYPE_ClientRPC] = IdGenerator.GetCurrentId();
		}
		else if (Group == RPC_Server)
		{
			SubobjectData.SchemaComponents[EComponentType::TYPE_ServerRPC] = IdGenerator.GetCurrentId();
		}
		else if (Group == RPC_NetMulticast)
		{
			SubobjectData.SchemaComponents[EComponentType::TYPE_NetMulticastRPC] = IdGenerator.GetCurrentId();
		}
		else if (Group == RPC_CrossServer)
		{
			SubobjectData.SchemaComponents[EComponentType::TYPE_CrossServerRPC] = IdGenerator.GetCurrentId();
		}
	}

	return SubobjectData;
}

void GenerateActorComponentSchemaForActor(FComponentIdGenerator& IdGenerator, UClass* ActorClass, TSharedPtr<FUnrealType> TypeInfo, FString SchemaPath, FSchemaData& ActorSchemaData)
{
	FCodeWriter Writer;

	Writer.Printf(R"""(
		// Copyright (c) Improbable Worlds Ltd, All Rights Reserved
		// Note that this file has been generated automatically
		package unreal.generated.%s.components;)""",
		*UnrealNameToSchemaTypeName(TypeInfo->Type->GetName().ToLower()));

	Writer.PrintNewLine();

	GenerateActorIncludes(Writer, TypeInfo);

	bool bHasComponents = false;
	TSet<UObject*> SeenComponents;
	int32 CurrentOffset = 1;

	for (auto& PropertyPair : TypeInfo->Properties)
	{
		UProperty* Property = PropertyPair.Key;
		UObjectProperty* ObjectProperty = Cast<UObjectProperty>(Property);

		TSharedPtr<FUnrealType>& PropertyTypeInfo = PropertyPair.Value->Type;

		if (ObjectProperty)
		{
			UObject* ContainerCDO = ActorClass->GetDefaultObject();
			UObject* Value = ObjectProperty->GetPropertyValue_InContainer(ContainerCDO);

			if (Value != nullptr && Value->GetOuter() == ContainerCDO && !Value->IsEditorOnly())
			{
				if (IsReplicatedActorComponent(PropertyTypeInfo) && !SeenComponents.Contains(Value))
				{
					bHasComponents = true;
					SeenComponents.Add(Value);

					FSubobjectSchemaData SubobjectData = GenerateActorComponentSpecificSchema(Writer, IdGenerator, Property->GetName(), PropertyTypeInfo, Value->GetClass());
					ActorSchemaData.SubobjectData.Add(CurrentOffset, SubobjectData);

					SchemaDatabase->ClassToSchema.Add(Value->GetClass(), FSchemaData());
				}

				CurrentOffset++;
			}
		}
	}

	if (bHasComponents)
	{
		Writer.WriteToFile(FString::Printf(TEXT("%s%sComponents.schema"), *SchemaPath, *UnrealNameToSchemaTypeName(ActorClass->GetName())));
	}
}

void GenerateActorIncludes(FCodeWriter& Writer, TSharedPtr<FUnrealType>& TypeInfo)
{
	TSet<UStruct*> AlreadyImported;

	bool bImportCoreTypes = false;

	for (auto& PropertyPair : TypeInfo->Properties)
	{
		UProperty* Property = PropertyPair.Key;
		UObjectProperty* ObjectProperty = Cast<UObjectProperty>(Property);

		TSharedPtr<FUnrealType>& PropertyTypeInfo = PropertyPair.Value->Type;

		if (ObjectProperty)
		{
			UObject* ContainerCDO = Cast<UClass>(TypeInfo->Type)->GetDefaultObject();
			UObject* Value = ObjectProperty->GetPropertyValue_InContainer(ContainerCDO);

			if (Value != nullptr && Value->GetOuter() == ContainerCDO && !Value->IsEditorOnly() && IsReplicatedActorComponent(PropertyTypeInfo))
			{
				bImportCoreTypes |= PropertyTypeInfo->RPCs.Num() > 0;

				if (!AlreadyImported.Contains(Value->GetClass()))
				{
					Writer.Printf("import \"unreal/generated/ActorComponents/%s.schema\";", *UnrealNameToSchemaTypeName(Value->GetClass()->GetName()));
					AlreadyImported.Add(Value->GetClass());
				}
			}
		}
	}

	if (bImportCoreTypes)
	{
		Writer.Printf("import \"unreal/gdk/core_types.schema\";");
	}
}
