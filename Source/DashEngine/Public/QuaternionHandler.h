#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "QuaternionHandler.generated.h"

/**
 *
 */
UCLASS()
class DASHENGINE_API UQuaternionHandler : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Quaternion to Rotator", Keywords = "Quaternion, Quat, Rot, Rotator"), Category = "Quaternion")
	static FRotator QuatToRotator(FQuat Quaternion);

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Rotator to Quaternion", Keywords = "Quaternion, Quat, Rot, Rotator"), Category = "Quaternion")
	static FQuat RotatorToQuat(FRotator Rotator);

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Quaternion from axis and angle", Keywords = "Quaternion, Quat, Rot, Rotator"), Category = "Quaternion")
	static FQuat QuatFromAngleAndAxis(FVector axis, float angle);

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Quaternion to vector", Keywords = "Quaternion, Quat, Vec, Vector"), Category = "Quaternion")
  	static FVector QuatToEuler(FQuat Quaternion);

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Quaternion multiply", Keywords = "Quaternion, Quat, Product, Mul, Multiply"), Category = "Quaternion")
	static FQuat QuatProduct(FQuat A, FQuat B);


};