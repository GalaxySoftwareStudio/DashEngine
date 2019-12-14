#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#if WITH_EDITORONLY_DATA
#include "Editor/UnrealEd/Classes/Editor/EditorEngine.h"
#include "Editor/UnrealEd/Public/Editor.h"
#include "Editor/ContentBrowser/Public/ContentBrowserModule.h"
#endif
#include "Runtime/Engine/Classes/Engine/ObjectLibrary.h"
#include "PlatformFunctions.generated.h"

UCLASS()
class DASHENGINE_API UPlatformFunctions : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
	
public:

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Get CPU Brand Name", Keywords = "CPU brand"), Category = Game)
		static FString GetCPUBrandName();

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Get CPU Vendor Name", Keywords = "CPU vendor"), Category = Game)
		static FString GetCPUVendorName();

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Get GPU Brand Name", Keywords = "GPU brand"), Category = Game)
		static FString GetGPUBrandName();

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Get Number of CPU Cores", Keywords = "CPU cores"), Category = Game)
		static int32 GetCPUCores();

#if WITH_EDITOR
	#pragma message("Including GetContentBrowserSelectedItems")
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Get Selected Items in Content Browser", Keywords = "content browser selected items", Category = ContentBrowser))
		static TArray<FAssetData> GetContentBrowserSelectedItems();
#endif

	UFUNCTION(BlueprintPure, meta = (DisplayName = "List All Blueprints In Path", Keywords = "content browser selected items blueprints list path get child of", Category = AssetData))
		static bool ListAllBlueprintsInPath(FName Path, TArray<UClass*>& Result, UClass* Class);
};