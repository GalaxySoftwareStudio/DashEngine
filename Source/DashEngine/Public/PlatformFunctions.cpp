#include "PlatformFunctions.h"
#include "DashEngine.h"

FString UPlatformFunctions::GetCPUBrandName()
{
	return FWindowsPlatformMisc::GetCPUBrand();
}

FString UPlatformFunctions::GetCPUVendorName()
{
	return FWindowsPlatformMisc::GetCPUVendor();
}

FString UPlatformFunctions::GetGPUBrandName()
{
	return FWindowsPlatformMisc::GetPrimaryGPUBrand();
}

int32 UPlatformFunctions::GetCPUCores()
{
	return FWindowsPlatformMisc::NumberOfCores();
}
#if WITH_EDITOR
TArray<FAssetData> UPlatformFunctions::GetContentBrowserSelectedItems()
{
	TArray<FAssetData> Assets;
	GEditor->GetContentBrowserSelections(Assets);
	return Assets;
}
#endif

bool UPlatformFunctions::ListAllBlueprintsInPath(FName Path, TArray<UClass*>& Result, UClass* Class)
{
	auto Library = UObjectLibrary::CreateLibrary(Class, true, GIsEditor);
	Library->LoadBlueprintAssetDataFromPath(Path.ToString());

	TArray<FAssetData> Assets;
	Library->GetAssetDataList(Assets);

	for (auto& Asset : Assets)
	{
		UBlueprint* bp = Cast<UBlueprint>(Asset.GetAsset());
		if (bp)
		{
			auto gc = bp->GeneratedClass;
			if (gc)
			{
				Result.Add(gc);
			}
		}
		else
		{
			auto GeneratedClassName = (Asset.AssetName.ToString() + "_C");

			UClass* Clazz = FindObject<UClass>(Asset.GetPackage(), *GeneratedClassName);
			if (Clazz)
			{
				Result.Add(Clazz);
			}
			else
			{
				UObjectRedirector* RenamedClassRedirector = FindObject<UObjectRedirector>(Asset.GetPackage(), *GeneratedClassName);
				if (RenamedClassRedirector)
				{
					Result.Add(CastChecked<UClass>(RenamedClassRedirector->DestinationObject));
				}
			}

		}
	}

	return true;
}