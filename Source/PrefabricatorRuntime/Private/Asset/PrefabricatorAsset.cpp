//$ Copyright 2015-19, Code Respawn Technologies Pvt Ltd - All Rights Reserved $//

#include "Asset/PrefabricatorAsset.h"

#include "Prefab/PrefabTools.h"
#include "Utils/PrefabricatorService.h"

#include "GameFramework/Actor.h"
#include "PrefabricatorSettings.h"
#include "Regex.h"
#include "PackageName.h"

DEFINE_LOG_CATEGORY_STATIC(LogPrefabricatorAsset, Log, All);

UPrefabricatorAsset::UPrefabricatorAsset(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {
	Version = (int32)EPrefabricatorAssetVersion::LatestVersion;
}

UPrefabricatorAsset* UPrefabricatorAsset::GetPrefabAsset(const FPrefabAssetSelectionConfig& InConfig)
{
	return this;
}

FVector FPrefabricatorAssetUtils::FindPivot(const TArray<AActor*>& InActors)
{
	FVector Pivot = FVector::ZeroVector;
	if (InActors.Num() > 0) {
		float LowestZ = MAX_flt;
		FBox Bounds(EForceInit::ForceInit);
		for (AActor* Actor : InActors) {
			FBox ActorBounds = FPrefabTools::GetPrefabBounds(Actor);
			Bounds += ActorBounds;
		}

		switch (GetDefault< UPrefabricatorSettings>()->PivotPosition) 
		{ 
			case EPrefabricatorPivotPosition::ExtremeLeft:
				Pivot = Bounds.GetCenter() - Bounds.GetExtent();
				break;
			case EPrefabricatorPivotPosition::ExtremeRight:
				Pivot = Bounds.GetCenter() + Bounds.GetExtent();
				break;
			case EPrefabricatorPivotPosition::Center:
				Pivot = Bounds.GetCenter();
				break;
			default:;
		}
		Pivot.Z = Bounds.Min.Z;
	}

	TSharedPtr<IPrefabricatorService> Service = FPrefabricatorService::Get();
	if (Service.IsValid()) {
		Pivot = Service->SnapToGrid(Pivot);
	}

	return Pivot;
}

EComponentMobility::Type FPrefabricatorAssetUtils::FindMobility(const TArray<AActor*>& InActors)
{
	return EComponentMobility::Static;
	/*
	EComponentMobility::Type Mobility = EComponentMobility::Movable;
	for (AActor* Actor : InActors) {
		if (!Actor || !Actor->GetRootComponent()) {
			continue;
		}
		EComponentMobility::Type ActorMobility = Actor->GetRootComponent()->Mobility;
		if (Mobility == EComponentMobility::Movable && ActorMobility == EComponentMobility::Stationary) {
			Mobility = EComponentMobility::Stationary;
		}
		else if (ActorMobility == EComponentMobility::Static) {
			Mobility = EComponentMobility::Static;
		}
	}

	return Mobility;
	*/
}

///////////////////////////////////////// UPrefabricatorAssetCollection ///////////////////////////////////////// 

UPrefabricatorAssetCollection::UPrefabricatorAssetCollection(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {
	Version = (int32)EPrefabricatorCollectionAssetVersion::LatestVersion;
}

UPrefabricatorAsset* UPrefabricatorAssetCollection::GetPrefabAsset(const FPrefabAssetSelectionConfig& InConfig)
{
	if (Prefabs.Num() == 0) return nullptr;

	float TotalWeight = 0.0f;
	for (const FPrefabricatorAssetCollectionItem& Item : Prefabs) {
		TotalWeight += FMath::Max(0.0f, Item.Weight);
	}

	FRandomStream Random;
	Random.Initialize(InConfig.Seed);
	
	TSoftObjectPtr<UPrefabricatorAsset> PrefabAssetPtr;

	if (TotalWeight == 0) {
		// Return a random value from the list
		int32 Index = Random.RandRange(0, Prefabs.Num() - 1);
		PrefabAssetPtr = Prefabs[Index].PrefabAsset;
	}
	else {
		float SelectionValue = Random.FRandRange(0, TotalWeight);
		float StartRange = 0.0f;
		for (const FPrefabricatorAssetCollectionItem& Item : Prefabs) {
			float EndRange = StartRange + Item.Weight;
			if (SelectionValue >= StartRange && SelectionValue < EndRange) {
				PrefabAssetPtr = Item.PrefabAsset;
				break;
			}
			StartRange = EndRange;
		}
		if (!PrefabAssetPtr.IsValid()) {
			PrefabAssetPtr = Prefabs.Last().PrefabAsset;
		}
	}
	if (PrefabAssetPtr.IsValid()) {
		return PrefabAssetPtr.LoadSynchronous();
	}
	return nullptr;
}

void UPrefabricatorEventListener::PostSpawn_Implementation(APrefabActor* Prefab)
{

}

void UPrefabricatorProperty::SaveReferencedAssetValues()
{
	AssetSoftReferenceMappings.Reset();

	const FRegexPattern Pattern(TEXT("[A-Za-z0-9_]+'.*?'"));
	FRegexMatcher Matcher(Pattern, *ExportedValue);

	while (Matcher.FindNext()) {
		int32 StartIdx = Matcher.GetMatchBeginning();
		int32 EndIdx = Matcher.GetMatchEnding();
		FString AssetPath = ExportedValue.Mid(StartIdx, EndIdx - StartIdx + 1);
		if (AssetPath.StartsWith("PrefabricatorAssetUserData")) {
			continue;
		}
		FSoftObjectPath SoftPath(AssetPath);

		FPrefabricatorPropertyAssetMapping Mapping;
		Mapping.AssetReference = SoftPath;
		if (Mapping.AssetReference.IsValid()) {
			FString ObjectPathString;
			FPackageName::ParseExportTextPath(AssetPath, &Mapping.AssetClassName, &ObjectPathString);
			Mapping.AssetObjectPath = *ObjectPathString;
			AssetSoftReferenceMappings.Add(Mapping);
			UE_LOG(LogPrefabricatorAsset, Log, TEXT("######>>> Found Asset: [%s][%s] | %s"), *Mapping.AssetClassName, *Mapping.AssetObjectPath.ToString(), *Mapping.AssetReference.GetAssetPathName().ToString());
		}
	}
}

void UPrefabricatorProperty::LoadReferencedAssetValues()
{
	bool bModified = false;
	for (FPrefabricatorPropertyAssetMapping& Mapping : AssetSoftReferenceMappings) {
		// Check if the name has changed
		//if (!Mapping.AssetReference.IsValid()) {
		//	continue;
		//}

		FName ReferencedPath = Mapping.AssetReference.GetAssetPathName();
		if (ReferencedPath.ToString().IsEmpty()) {
			continue;
		}

		if (ReferencedPath == Mapping.AssetObjectPath) {
			// No change in the exported text path and the referenced path
			continue;
		}

		// The object path has changed.  Update it and mark as modified
		FString ReplaceFrom = FString::Printf(TEXT("%s\'%s\'"), *Mapping.AssetClassName, *Mapping.AssetObjectPath.ToString());
		FString ReplaceTo = FString::Printf(TEXT("%s\'%s\'"), *Mapping.AssetClassName, *ReferencedPath.ToString());
		ExportedValue = ExportedValue.Replace(*ReplaceFrom, *ReplaceTo);
		Mapping.AssetObjectPath = ReferencedPath;

		bModified = true;
	}

	if (bModified) {
		Modify();
	}
}
