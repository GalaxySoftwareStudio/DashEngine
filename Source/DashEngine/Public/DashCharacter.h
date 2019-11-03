// Copyright 2019 Edited by GalaxySoftware for DashEngine 1.0.5
// Read LICENSE file for details about permission to use this software. //
// Uses code from Epic Games:
// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.


#pragma once

#include "GameFramework/Character.h"
#include "DashCharacter.generated.h"


/**
* Pawns are the physical representations of players and creatures in a level.
* Characters are Pawns that have a mesh, collision, and physics.
*/
UCLASS()
class DASHENGINE_API ADashCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ADashCharacter(const FObjectInitializer& ObjectInitializer);

protected:
	/** Allows a Pawn to set up custom input bindings. Called upon possession by a PlayerController, using the InputComponent created by CreatePlayerInputComponent(). */
	virtual void SetupPlayerInputComponent(UInputComponent* InputComponent) override;

public:
	/** Apply momentum caused by damage. */
	virtual void ApplyDamageMomentum(float DamageTaken, const FDamageEvent& DamageEvent, APawn* PawnInstigator, AActor* DamageCauser) override;

public:
	/** @return	Pawn's eye location */
	virtual FVector GetPawnViewLocation() const override;

public:
	/** Update location and rotation from ReplicatedMovement. Not called for simulated physics! */
	virtual void PostNetReceiveLocationAndRotation() override;

public:
	/**
	* Set a pending launch velocity on the Character. This velocity will be processed on the next
	* CharacterMovementComponent tick, and will set it to the "falling" state. Triggers the OnLaunched event.
	* @note This version has a different behavior for the boolean parameters that take into account the Character's orientation.
	* @param LaunchVelocity is the velocity to impart to the Character.
	* @param bHorizontalOverride if true replace the horizontal part of the Character's velocity instead of adding to it.
	* @param bVerticalOverride if true replace the vertical part of the Character's velocity instead of adding to it.
	*/
	UFUNCTION(Category = "Pawn|DashCharacter", BlueprintCallable)
		virtual void LaunchCharacterRotated(FVector LaunchVelocity, bool bHorizontalOverride, bool bVerticalOverride);

public:
	/** Returns DashCharacterMovement subobject **/
	FORCEINLINE class UDashCharacterMovementComponent* GetDashCharacterMovement() const;

public:
	/** Axis name for "move forward/back" control. This should match an Axis Binding in your input settings */
	UPROPERTY(Category = "Dash Character", BlueprintReadOnly, EditDefaultsOnly)
		FString MoveForwardAxisName;

	/** Axis name for "move left/right" control. This should match an Axis Binding in your input settings */
	UPROPERTY(Category = "Dash Character", BlueprintReadOnly, EditDefaultsOnly)
		FString MoveRightAxisName;

public:
	/**
	* Input handler for depth controls.
	*/
	UFUNCTION(Category = "Pawn|DashCharacter", BlueprintCallable)
		virtual void DashMoveForward(float Value);

public:
	/**
	* Input handler for side controls.
	*/
	UFUNCTION(Category = "Pawn|DashCharacter", BlueprintCallable)
		virtual void DashMoveRight(float Value);

public:
	/**
	* If true, the forward and right vectors of the character will be used for moving instead of the camera vectors.
	*/
	UPROPERTY(Category = "Dash Character Movement", BlueprintReadWrite, EditAnywhere)
		uint32 bUseCharacterVectors : 1;
};
