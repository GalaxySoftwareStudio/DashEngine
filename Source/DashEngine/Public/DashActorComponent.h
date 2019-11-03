

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DashActorComponent.generated.h"


UCLASS( ClassGroup=(DashEngine), meta=(BlueprintSpawnableComponent), Blueprintable, BlueprintType)
class DASHENGINE_API UDashActorComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	// Sets default values for this component's properties
	UDashActorComponent();
	EWorldType::Type CWType;

protected:
	// Called when the game starts
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void PostInitProperties() override;

public:
	// Reproduction of construction script
	UFUNCTION(BlueprintNativeEvent)
	void OnConstructed_InEditor();

	UFUNCTION(BlueprintNativeEvent)
	void OnConstructed_InGame();

};