// Copyright 2019 Edited by GalaxySoftware for DashEngine
// Read LICENSE file for details about permission to use this software. //
// Uses code from Epic Games:
// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.


#include "DashCharacterMovementComponent.h"
#include "DashEngine.h"

#include "DrawDebugHelpers.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Components/CapsuleComponent.h"
#include "Components/BrushComponent.h"
#include "GameFramework/MovementComponent.h"
#include "GameFramework/NavMovementComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PhysicsVolume.h"
#include "GameFramework/GameNetworkManager.h"
#include "AI/Navigation/PathFollowingAgentInterface.h"
#include "AI/Navigation/AvoidanceManager.h"
#include "Navigation/PathFollowingComponent.h" // @todo Epic: this is here only due to circular dependency to AIModule.
//#include "DestructibleInterface.h"
#include "DestructibleComponent.h"
#include "Engine/Canvas.h"
#include "Net/PerfCountersHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogCharacterMovement, Log, All);

// Character stats.
DECLARE_CYCLE_STAT(TEXT("Char RootMotionSource Apply"), STAT_CharacterMovementRootMotionSourceApply, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char StepUp"), STAT_CharStepUp, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char AdjustFloorHeight"), STAT_CharAdjustFloorHeight, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char PhysWalking"), STAT_CharPhysWalking, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char PhysFalling"), STAT_CharPhysFalling, STATGROUP_Character);

// Magic numbers.
const float MAX_STEP_SIDE_Z = 0.08f; // Maximum Z value for the normal on the vertical side of steps.
const float SWIMBOBSPEED = -80.0f;
const float VERTICAL_SLOPE_NORMAL_Z = 0.001f; // Slope is vertical if Abs(Normal.Z) <= this threshold. Accounts for precision problems that sometimes angle normals slightly off horizontal for vertical surface.

											  // Statics.
namespace DashCharacterMovementComponentStatics
{
	static const FName CrouchTraceName = FName(TEXT("CrouchTrace"));
	static const FName CheckLedgeDirectionName = FName(TEXT("CheckLedgeDirection"));
	static const FName CheckWaterJumpName = FName(TEXT("CheckWaterJump"));
	static const FName ComputeFloorDistName = FName(TEXT("ComputeFloorDistSweep"));
	static const FName FloorLineTraceName = FName(TEXT("ComputeFloorDistLineTrace"));
	static const FName ImmersionDepthName = FName(TEXT("MovementComp_Character_ImmersionDepth"));
}

// CVars.
namespace DashCharacterMovementCVars
{
#if !UE_BUILD_SHIPPING
	static int32 NetShowCorrections = 0;
	FAutoConsoleVariableRef CVarNetShowCorrections(
		TEXT("p.NetShowCorrections"),
		NetShowCorrections,
		TEXT("Whether to draw client position corrections (red is incorrect, green is corrected).\n")
		TEXT("0: Disable, 1: Enable"),
		ECVF_Cheat);

	static float NetCorrectionLifetime = 4.f;
	FAutoConsoleVariableRef CVarNetCorrectionLifetime(
		TEXT("p.NetCorrectionLifetime"),
		NetCorrectionLifetime,
		TEXT("How long a visualized network correction persists.\n")
		TEXT("Time in seconds each visualized network correction persists."),
		ECVF_Cheat);
#endif // !UE_BUILD_SHIPPING
}


UDashCharacterMovementComponent::UDashCharacterMovementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAlignComponentToFloor = false;
	bAlignComponentToGravity = false;
	bAlignCustomGravityToFloor = false;
	bDirtyCustomGravityDirection = false;
	bDisableGravityReplication = false;
	bIgnoreBaseRollMove = false;
	CustomGravityDirection = FVector::ZeroVector;
	GravityPoint = FVector::ZeroVector;
	OldGravityPoint = GravityPoint;
	OldGravityScale = GravityScale;
	
}

bool UDashCharacterMovementComponent::DoJump(bool bReplayingMoves)
{
	if (CharacterOwner && CharacterOwner->CanJump())
	{
		const FVector JumpDir = GetComponentAxisZ();

		// If movement isn't constrained or the angle between plane normal and jump direction is between 60 and 120 degrees...
		if (!bConstrainToPlane || FMath::Abs(PlaneConstraintNormal | JumpDir) < 0.5f)
		{
			// Set to zero the vertical component of velocity.
			Velocity = FVector::VectorPlaneProject(Velocity, JumpDir);

			// Perform jump.
			Velocity += JumpDir * JumpZVelocity;
			SetMovementMode(MOVE_Falling);

			return true;
		}
	}

	return false;
}

FVector UDashCharacterMovementComponent::GetImpartedMovementBaseVelocity() const
{
	FVector Result = FVector::ZeroVector;
	if (CharacterOwner)
	{
		UPrimitiveComponent* MovementBase = CharacterOwner->GetMovementBase();
		if (MovementBaseUtility::IsDynamicBase(MovementBase))
		{
			FVector BaseVelocity = MovementBaseUtility::GetMovementBaseVelocity(MovementBase, CharacterOwner->GetBasedMovement().BoneName);

			if (bImpartBaseAngularVelocity)
			{
				const FVector CharacterBasePosition = (UpdatedComponent->GetComponentLocation() - GetComponentAxisZ() * CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
				const FVector BaseTangentialVel = MovementBaseUtility::GetMovementBaseTangentialVelocity(MovementBase, CharacterOwner->GetBasedMovement().BoneName, CharacterBasePosition);
				BaseVelocity += BaseTangentialVel;
			}

			if (bImpartBaseVelocityX)
			{
				Result.X = BaseVelocity.X;
			}
			if (bImpartBaseVelocityY)
			{
				Result.Y = BaseVelocity.Y;
			}
			if (bImpartBaseVelocityZ)
			{
				Result.Z = BaseVelocity.Z;
			}
		}
	}

	return Result;
}

void UDashCharacterMovementComponent::JumpOff(AActor* MovementBaseActor)
{
	if (!bPerformingJumpOff)
	{
		bPerformingJumpOff = true;

		if (CharacterOwner)
		{
			const float MaxSpeed = GetMaxSpeed() * 0.85f;
			Velocity += GetBestDirectionOffActor(MovementBaseActor) * MaxSpeed;

			const FVector JumpDir = GetComponentAxisZ();
			FVector Velocity2D = FVector::VectorPlaneProject(Velocity, JumpDir);

			if (Velocity2D.Size() > MaxSpeed)
			{
				Velocity2D = FVector::VectorPlaneProject(Velocity.GetSafeNormal() * MaxSpeed, JumpDir);
			}

			Velocity = Velocity2D + JumpDir * (JumpZVelocity * JumpOffJumpZFactor);
			SetMovementMode(MOVE_Falling);
		}

		bPerformingJumpOff = false;
	}
}

FVector UDashCharacterMovementComponent::GetBestDirectionOffActor(AActor* BaseActor) const
{
	// By default, just pick a random direction. Derived character classes can choose to do more complex calculations,
	// such as finding the shortest distance to move in based on the BaseActor's bounding volume.
	const float RandAngle = FMath::DegreesToRadians(GetNetworkSafeRandomAngleDegrees());
	const FQuat PawnRotation = UpdatedComponent->GetComponentQuat();
	return PawnRotation.RotateVector(FVector(FMath::Cos(RandAngle), FMath::Sin(RandAngle), 0.5f).GetSafeNormal());
}

void UDashCharacterMovementComponent::SetDefaultMovementMode()
{
	// Check for water volume.
	if (CanEverSwim() && IsInWater())
	{
		SetMovementMode(DefaultWaterMovementMode);
	}
	else if (!CharacterOwner || MovementMode != DefaultLandMovementMode)
	{
		const FVector SavedVelocity = Velocity;
		SetMovementMode(DefaultLandMovementMode);

		// Avoid 1-frame delay if trying to walk but walking fails at this location.
		if (MovementMode == MOVE_Walking && GetMovementBase() == NULL)
		{
			Velocity = SavedVelocity; // Prevent temporary walking state from modifying velocity.
			SetMovementMode(MOVE_Falling);
		}
	}
}

void UDashCharacterMovementComponent::OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode)
{
	if (!HasValidData())
	{
		return;
	}

	// Update collision settings if needed.
	if (MovementMode == MOVE_NavWalking)
	{
		SetNavWalkingPhysics(true);
		//		GroundMovementMode = MovementMode;
		SetGroundMovementMode(MovementMode); //OVERRIDEN

											 // @todo arbitrary-gravity: NavWalking not supported.
											 // Walking uses only XY velocity.
		Velocity.Z = 0.0f;
	}
	else if (PreviousMovementMode == MOVE_NavWalking)
	{
		if (MovementMode == DefaultLandMovementMode || IsWalking())
		{
			const bool bSucceeded = TryToLeaveNavWalking();
			if (!bSucceeded)
			{
				return;
			}
		}
		else
		{
			SetNavWalkingPhysics(false);
		}
	}

	// React to changes in the movement mode.
	if (MovementMode == MOVE_Walking)
	{
		// Walking must be on a walkable floor, with a base.
		bCrouchMaintainsBaseLocation = true;
		//		GroundMovementMode = MovementMode;
		SetGroundMovementMode(MovementMode); //OVERRIDEN

											 // Make sure we update our new floor/base on initial entry of the walking physics.
		FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, false);
		UpdateComponentRotation();
		AdjustFloorHeight();
		SetBaseFromFloor(CurrentFloor);

		// Walking uses only horizontal velocity.
		MaintainHorizontalGroundVelocity();
	}
	else
	{
		CurrentFloor.Clear();
		bCrouchMaintainsBaseLocation = false;

		UpdateComponentRotation();

		if (MovementMode == MOVE_Falling)
		{
			Velocity += GetImpartedMovementBaseVelocity();
			CharacterOwner->Falling();
		}

		SetBase(NULL);

		if (MovementMode == MOVE_None)
		{
			// Kill velocity and clear queued up events.
			StopMovementKeepPathing();
			CharacterOwner->ClearJumpInput(GetWorld()->GetDeltaSeconds());
		}
	}
	if (MovementMode == MOVE_Falling && PreviousMovementMode != MOVE_Falling)
	{
		IPathFollowingAgentInterface* PFAgent = GetPathFollowingAgent();
		if (PFAgent)
		{
			PFAgent->OnStartedFalling();
		}
	}
	CharacterOwner->OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);
	//	ensure(GroundMovementMode == MOVE_Walking || GroundMovementMode == MOVE_NavWalking);
	ensure(GetGroundMovementMode() == MOVE_Walking || GetGroundMovementMode() == MOVE_NavWalking); //OVERRIDEN
}

void UDashCharacterMovementComponent::PerformAirControlForPathFollowing(FVector Direction, float ZDiff)
{
	// Abort if no valid gravity can be obtained.
	const FVector GravityDir = GetGravityDirection();
	if (GravityDir.IsZero())
	{
		return;
	}

	PerformAirControlForPathFollowingEx(Direction, GravityDir);
}

void UDashCharacterMovementComponent::PerformAirControlForPathFollowingEx(const FVector& MoveVelocity, const FVector& GravDir)
{
	const float MoveSpeedZ = (MoveVelocity | GravDir) * -1.0f;

	// Use air control if low grav or above destination and falling towards it.
	if (CharacterOwner && (Velocity | GravDir) > 0.0f && (MoveSpeedZ < 0.0f || GetGravityMagnitude() < FMath::Abs(0.9f * GetWorld()->GetDefaultGravityZ())))
	{
		if (MoveSpeedZ < 0.0f)
		{
			const FVector Velocity2D = FVector::VectorPlaneProject(Velocity, GravDir);
			if (Velocity2D.SizeSquared() == 0.0f)
			{
				Acceleration = FVector::ZeroVector;
			}
			else
			{
				const float Dist2D = FVector::VectorPlaneProject(MoveVelocity, GravDir).Size();
				Acceleration = MoveVelocity.GetSafeNormal() * GetMaxAcceleration();

				if (Dist2D < 0.5f * FMath::Abs(MoveSpeedZ) && (Velocity | MoveVelocity) > 0.5f * FMath::Square(Dist2D))
				{
					Acceleration *= -1.0f;
				}

				if (Dist2D < 1.5f * CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius())
				{
					Velocity = GravDir * (Velocity | GravDir);
					Acceleration = FVector::ZeroVector;
				}
				else if ((Velocity | MoveVelocity) < 0.0f)
				{
					const float M = FMath::Max(0.0f, 0.2f - GetWorld()->DeltaTimeSeconds);
					Velocity = Velocity2D * M + GravDir * (Velocity | GravDir);
				}
			}
		}
	}
}

FVector UDashCharacterMovementComponent::ConstrainAnimRootMotionVelocity(const FVector& RootMotionVelocity, const FVector& CurrentVelocity) const
{
	FVector Result = RootMotionVelocity;

	// Do not override vertical velocity if in falling physics, we want to keep the effect of gravity.
	if (IsFalling())
	{
		const FVector GravityDir = GetGravityDirection(true);
		Result = FVector::VectorPlaneProject(Result, GravityDir) + GravityDir * (CurrentVelocity | GravityDir);
	}

	return Result;
}

void UDashCharacterMovementComponent::SimulateMovement(float DeltaSeconds)
{
	if (!HasValidData() || UpdatedComponent->Mobility != EComponentMobility::Movable || UpdatedComponent->IsSimulatingPhysics())
	{
		return;
	}

	const bool bIsSimulatedProxy = (CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy);

	// Workaround for replication not being updated initially.
	if (bIsSimulatedProxy &&
		CharacterOwner->GetReplicatedBasedMovement().Location.IsZero() &&
		CharacterOwner->GetReplicatedBasedMovement().Rotation.IsZero() &&
		CharacterOwner->GetReplicatedBasedMovement().MovementBase->GetPhysicsLinearVelocity().IsZero())
	{
		return;
	}

	// If base is not resolved on the client, we should not try to simulate at all.
	if (CharacterOwner->GetReplicatedBasedMovement().IsBaseUnresolved())
	{
		UE_LOG(LogCharacterMovement, Verbose, TEXT("Base for simulated character '%s' is not resolved on client, skipping SimulateMovement"), *CharacterOwner->GetName());
		return;
	}

	FVector OldVelocity;
	FVector OldLocation;

	// Scoped updates can improve performance of multiple MoveComponent calls.
	{
		FScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);

		if (bIsSimulatedProxy)
		{
			// Handle network changes.
			if (bNetworkUpdateReceived)
			{
				bNetworkUpdateReceived = false;
				if (bNetworkMovementModeChanged)
				{
					bNetworkMovementModeChanged = false;
					ApplyNetworkMovementMode(CharacterOwner->GetReplicatedMovementMode());
				}
				else if (bJustTeleported)
				{
					// Make sure floor is current. We will continue using the replicated base, if there was one.
					bJustTeleported = false;
					UpdateFloorFromAdjustment();
				}
			}

			HandlePendingLaunch();
		}

		if (MovementMode == MOVE_None)
		{
			return;
		}

		// Both not currently used for simulated movement.
		Acceleration = Velocity.GetSafeNormal();
		AnalogInputModifier = 1.0f;

		MaybeUpdateBasedMovement(DeltaSeconds);

		// Simulated pawns predict location.
		OldVelocity = Velocity;
		OldLocation = UpdatedComponent->GetComponentLocation();
		FStepDownResult StepDownResult;
		MoveSmooth(Velocity, DeltaSeconds, &StepDownResult);

		// Consume path following requested velocity.
		bHasRequestedVelocity = false;

		// Find floor and check if falling.
		if (IsMovingOnGround() || MovementMode == MOVE_Falling)
		{
			const bool bSimGravityDisabled = (CharacterOwner->bSimGravityDisabled && bIsSimulatedProxy);
			const FVector Gravity = GetGravity();

			if (StepDownResult.bComputedFloor)
			{
				CurrentFloor = StepDownResult.FloorResult;
			}
			else if (IsMovingOnGround() || (!Gravity.IsZero() && (Velocity | Gravity) >= 0.0f))
			{
				FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, Velocity.IsZero(), NULL);
			}
			else
			{
				CurrentFloor.Clear();
			}

			if (!CurrentFloor.IsWalkableFloor())
			{
				if (!bSimGravityDisabled)
				{
					// No floor, must fall.
					Velocity = NewFallVelocity(Velocity, Gravity, DeltaSeconds);
				}
				SetMovementMode(MOVE_Falling);
			}
			else
			{
				// Walkable floor.
				if (IsMovingOnGround())
				{
					AdjustFloorHeight();
					SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);
				}
				else if (MovementMode == MOVE_Falling)
				{
					if (CurrentFloor.FloorDist <= MIN_FLOOR_DIST || (bSimGravityDisabled && CurrentFloor.FloorDist <= MAX_FLOOR_DIST))
					{
						// Landed.
						SetPostLandedPhysics(CurrentFloor.HitResult);
					}
					else
					{
						if (!bSimGravityDisabled)
						{
							// Continue falling.
							Velocity = NewFallVelocity(Velocity, Gravity, DeltaSeconds);
						}
						CurrentFloor.Clear();
					}
				}
			}
		}

		OnMovementUpdated(DeltaSeconds, OldLocation, OldVelocity);
	} // End scoped movement update.

	  // Call custom post-movement events. These happen after the scoped movement completes in case the events want to use the current state of overlaps etc.
	CallMovementUpdateDelegate(DeltaSeconds, OldLocation, OldVelocity);

	MaybeSaveBaseLocation();
	UpdateComponentVelocity();
	bJustTeleported = false;

	LastUpdateLocation = UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector;
	LastUpdateRotation = UpdatedComponent ? UpdatedComponent->GetComponentQuat() : FQuat::Identity;
	LastUpdateVelocity = Velocity;
}

void UDashCharacterMovementComponent::MaybeUpdateBasedMovement(float DeltaSeconds)
{
	UpdateGravity(DeltaSeconds);

	Super::MaybeUpdateBasedMovement(DeltaSeconds);
}

void UDashCharacterMovementComponent::UpdateBasedMovement(float DeltaSeconds)
{
	if (!HasValidData())
	{
		return;
	}

	const UPrimitiveComponent* MovementBase = CharacterOwner->GetMovementBase();
	if (!MovementBaseUtility::UseRelativeLocation(MovementBase))
	{
		return;
	}

	if (!IsValid(MovementBase) || !IsValid(MovementBase->GetOwner()))
	{
		SetBase(NULL);
		return;
	}

	// Ignore collision with bases during these movements.
	TGuardValue<EMoveComponentFlags> ScopedFlagRestore(MoveComponentFlags, MoveComponentFlags | MOVECOMP_IgnoreBases);

	FQuat DeltaQuat = FQuat::Identity;
	FVector DeltaPosition = FVector::ZeroVector;

	FQuat NewBaseQuat;
	FVector NewBaseLocation;
	if (!MovementBaseUtility::GetMovementBaseTransform(MovementBase, CharacterOwner->GetBasedMovement().BoneName, NewBaseLocation, NewBaseQuat))
	{
		return;
	}

	// Find change in rotation.
	const bool bRotationChanged = !OldBaseQuat.Equals(NewBaseQuat, 1e-8f);
	if (bRotationChanged)
	{
		DeltaQuat = NewBaseQuat * OldBaseQuat.Inverse();
	}

	// Only if base moved.
	if (bRotationChanged || (OldBaseLocation != NewBaseLocation))
	{
		// Calculate new transform matrix of base actor (ignoring scale).
		const FQuatRotationTranslationMatrix OldLocalToWorld(OldBaseQuat, OldBaseLocation);
		const FQuatRotationTranslationMatrix NewLocalToWorld(NewBaseQuat, NewBaseLocation);

		if (CharacterOwner->IsMatineeControlled())
		{
			FRotationTranslationMatrix HardRelMatrix(CharacterOwner->GetBasedMovement().Rotation, CharacterOwner->GetBasedMovement().Location);
			const FMatrix NewWorldTM = HardRelMatrix * NewLocalToWorld;
			const FQuat NewWorldRot = bIgnoreBaseRotation ? UpdatedComponent->GetComponentQuat() : NewWorldTM.ToQuat();
			MoveUpdatedComponent(NewWorldTM.GetOrigin() - UpdatedComponent->GetComponentLocation(), NewWorldRot, true);
		}
		else
		{
			FQuat FinalQuat = UpdatedComponent->GetComponentQuat();

			if (bRotationChanged && !bIgnoreBaseRotation)
			{
				// Apply change in rotation and pipe through FaceRotation to maintain axis restrictions.
				const FQuat PawnOldQuat = UpdatedComponent->GetComponentQuat();
				const FQuat TargetQuat = DeltaQuat * FinalQuat;
				FRotator TargetRotator(TargetQuat);
				CharacterOwner->FaceRotation(TargetRotator, 0.0f);
				FinalQuat = UpdatedComponent->GetComponentQuat();

				if (PawnOldQuat.Equals(FinalQuat, 1e-6f))
				{
					// Nothing changed. This means we probably are using another rotation mechanism (bOrientToMovement etc). We should still follow the base object.
					if (bOrientRotationToMovement || (bUseControllerDesiredRotation && CharacterOwner->Controller))
					{
						TargetRotator = ConstrainComponentRotation(TargetRotator);
						MoveUpdatedComponent(FVector::ZeroVector, TargetRotator, false);
						FinalQuat = UpdatedComponent->GetComponentQuat();
					}
				}

				// Pipe through ControlRotation, to affect camera.
				if (CharacterOwner->Controller)
				{
					const FQuat PawnDeltaRotation = FinalQuat * PawnOldQuat.Inverse();
					FRotator FinalRotation = FinalQuat.Rotator();
					UpdateBasedRotation(FinalRotation, PawnDeltaRotation.Rotator());
					FinalQuat = UpdatedComponent->GetComponentQuat();
				}
			}

			// We need to offset the base of the character here, not its origin, so offset by half height.
			float HalfHeight, Radius;
			CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(Radius, HalfHeight);

			const FVector BaseOffset = GetComponentAxisZ() * HalfHeight;
			const FVector LocalBasePos = OldLocalToWorld.InverseTransformPosition(UpdatedComponent->GetComponentLocation() - BaseOffset);
			const FVector NewWorldPos = ConstrainLocationToPlane(NewLocalToWorld.TransformPosition(LocalBasePos) + BaseOffset);
			DeltaPosition = ConstrainDirectionToPlane(NewWorldPos - UpdatedComponent->GetComponentLocation());

			// Move attached actor.
			if (bFastAttachedMove)
			{
				// We're trusting no other obstacle can prevent the move here.
				UpdatedComponent->SetWorldLocationAndRotation(NewWorldPos, FinalQuat, false);
			}
			else
			{
				/* @todo arbitrary-gravity: ugly code that might go away, not worth my time.
				// hack - transforms between local and world space introducing slight error FIXMESTEVE - discuss with engine team: just skip the transforms if no rotation?
				FVector BaseMoveDelta = NewBaseLocation - OldBaseLocation;
				if (!bRotationChanged && (BaseMoveDelta.X == 0.f) && (BaseMoveDelta.Y == 0.f))
				{
				DeltaPosition.X = 0.f;
				DeltaPosition.Y = 0.f;
				}
				*/
				FHitResult MoveOnBaseHit(1.0f);
				const FVector OldLocation = UpdatedComponent->GetComponentLocation();
				MoveUpdatedComponent(DeltaPosition, FinalQuat, true, &MoveOnBaseHit);
				if (!((UpdatedComponent->GetComponentLocation() - (OldLocation + DeltaPosition)).IsNearlyZero()))
				{
					OnUnableToFollowBaseMove(DeltaPosition, OldLocation, MoveOnBaseHit);
				}
			}
		}

		if (MovementBase->IsSimulatingPhysics() && CharacterOwner->GetMesh())
		{
			CharacterOwner->GetMesh()->ApplyDeltaToAllPhysicsTransforms(DeltaPosition, DeltaQuat);
		}
	}
}

void UDashCharacterMovementComponent::UpdateBasedRotation(FRotator& FinalRotation, const FRotator& ReducedRotation)
{
	AController* Controller = CharacterOwner ? CharacterOwner->Controller : NULL;
	float ControllerRoll = 0.0f;
	if (Controller && !bIgnoreBaseRotation)
	{
		const FRotator ControllerRot = Controller->GetControlRotation();
		ControllerRoll = ControllerRot.Roll;
		Controller->SetControlRotation(ControllerRot + ReducedRotation);
	}

	if (bIgnoreBaseRollMove)
	{
		// Remove roll.
		FinalRotation.Roll = 0.0f;
		if (Controller)
		{
			FinalRotation.Roll = UpdatedComponent->GetComponentRotation().Roll;
			FRotator NewRotation = Controller->GetControlRotation();
			NewRotation.Roll = ControllerRoll;
			Controller->SetControlRotation(NewRotation);
		}
	}
}

void UDashCharacterMovementComponent::Crouch(bool bClientSimulation)
{
	if (!HasValidData())
	{
		return;
	}

	if (!CanCrouchInCurrentState())
	{
		return;
	}

	// See if collision is already at desired size.
	if (CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() == CrouchedHalfHeight)
	{
		if (!bClientSimulation)
		{
			CharacterOwner->bIsCrouched = true;
		}
		CharacterOwner->OnStartCrouch(0.0f, 0.0f);
		return;
	}

	if (bClientSimulation && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		// Restore collision size before crouching.
		ACharacter* DefaultCharacter = CharacterOwner->GetClass()->GetDefaultObject<ACharacter>();
		CharacterOwner->GetCapsuleComponent()->SetCapsuleSize(DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight());
		bShrinkProxyCapsule = true;
	}

	// Change collision size to crouching dimensions.
	const float ComponentScale = CharacterOwner->GetCapsuleComponent()->GetShapeScale();
	const float OldUnscaledHalfHeight = CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float OldUnscaledRadius = CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleRadius();
	// Height is not allowed to be smaller than radius.
	const float ClampedCrouchedHalfHeight = FMath::Max3(0.0f, OldUnscaledRadius, CrouchedHalfHeight);
	CharacterOwner->GetCapsuleComponent()->SetCapsuleSize(OldUnscaledRadius, ClampedCrouchedHalfHeight);
	float HalfHeightAdjust = (OldUnscaledHalfHeight - ClampedCrouchedHalfHeight);
	float ScaledHalfHeightAdjust = HalfHeightAdjust * ComponentScale;

	if (!bClientSimulation)
	{
		const FVector CapsuleDown = GetComponentAxisZ() * -1.0f;

		// Crouching to a larger height? (this is rare).
		if (ClampedCrouchedHalfHeight > OldUnscaledHalfHeight)
		{
			FCollisionQueryParams CapsuleParams(DashCharacterMovementComponentStatics::CrouchTraceName, false, CharacterOwner);
			FCollisionResponseParams ResponseParam;
			InitCollisionParams(CapsuleParams, ResponseParam);
			const bool bEncroached = GetWorld()->OverlapBlockingTestByChannel(UpdatedComponent->GetComponentLocation() + CapsuleDown * ScaledHalfHeightAdjust,
				UpdatedComponent->GetComponentQuat(), UpdatedComponent->GetCollisionObjectType(), GetPawnCapsuleCollisionShape(SHRINK_None), CapsuleParams, ResponseParam);

			// If encroached, cancel.
			if (bEncroached)
			{
				CharacterOwner->GetCapsuleComponent()->SetCapsuleSize(OldUnscaledRadius, OldUnscaledHalfHeight);
				return;
			}
		}

		if (bCrouchMaintainsBaseLocation)
		{
			// Intentionally not using MoveUpdatedComponent, where a horizontal plane constraint would prevent the base of the capsule from staying at the same spot.
			UpdatedComponent->MoveComponent(CapsuleDown * ScaledHalfHeightAdjust, UpdatedComponent->GetComponentQuat(), true);
		}

		CharacterOwner->bIsCrouched = true;
	}

	bForceNextFloorCheck = true;

	// OnStartCrouch takes the change from the Default size, not the current one (though they are usually the same).
	const float MeshAdjust = ScaledHalfHeightAdjust;
	ACharacter* DefaultCharacter = CharacterOwner->GetClass()->GetDefaultObject<ACharacter>();
	HalfHeightAdjust = (DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() - ClampedCrouchedHalfHeight);
	ScaledHalfHeightAdjust = HalfHeightAdjust * ComponentScale;

	AdjustProxyCapsuleSize();
	CharacterOwner->OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);

	// Don't smooth this change in mesh position.
	if (bClientSimulation && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
		if (ClientData)
		{
			const FVector CapsuleDown = GetComponentAxisZ() * -1.0f;
			const float MeshTranslationOffsetZ = ClientData->MeshTranslationOffset | CapsuleDown;
			if (MeshTranslationOffsetZ != 0.0f)
			{
				ClientData->MeshTranslationOffset += CapsuleDown * MeshAdjust;
				ClientData->OriginalMeshTranslationOffset = ClientData->MeshTranslationOffset;
			}
		}
	}
}

void UDashCharacterMovementComponent::UnCrouch(bool bClientSimulation)
{
	if (!HasValidData())
	{
		return;
	}

	ACharacter* DefaultCharacter = CharacterOwner->GetClass()->GetDefaultObject<ACharacter>();

	// See if collision is already at desired size.
	if (CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() == DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight())
	{
		if (!bClientSimulation)
		{
			CharacterOwner->bIsCrouched = false;
		}
		CharacterOwner->OnEndCrouch(0.0f, 0.0f);
		return;
	}

	const float CurrentCrouchedHalfHeight = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

	const float ComponentScale = CharacterOwner->GetCapsuleComponent()->GetShapeScale();
	const float OldUnscaledHalfHeight = CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float HalfHeightAdjust = DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() - OldUnscaledHalfHeight;
	const float ScaledHalfHeightAdjust = HalfHeightAdjust * ComponentScale;
	const FVector PawnLocation = UpdatedComponent->GetComponentLocation();

	// Grow to uncrouched size.
	check(CharacterOwner->GetCapsuleComponent());
	bool bUpdateOverlaps = false;
	CharacterOwner->GetCapsuleComponent()->SetCapsuleSize(DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), bUpdateOverlaps);

	if (!bClientSimulation)
	{
		// Try to stay in place and see if the larger capsule fits. We use a slightly taller capsule to avoid penetration.
		const float SweepInflation = KINDA_SMALL_NUMBER * 10.0f;
		const FQuat PawnRotation = UpdatedComponent->GetComponentQuat();
		const FVector CapsuleDown = GetComponentAxisZ() * -1.0f;
		FCollisionQueryParams CapsuleParams(DashCharacterMovementComponentStatics::CrouchTraceName, false, CharacterOwner);
		FCollisionResponseParams ResponseParam;
		InitCollisionParams(CapsuleParams, ResponseParam);
		const FCollisionShape StandingCapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_HeightCustom, -SweepInflation); // Shrink by negative amount, so actually grow it.
		const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();
		bool bEncroached = true;

		if (!bCrouchMaintainsBaseLocation)
		{
			// Expand in place
			bEncroached = GetWorld()->OverlapBlockingTestByChannel(PawnLocation, PawnRotation, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);

			if (bEncroached)
			{
				// Try adjusting capsule position to see if we can avoid encroachment.
				if (ScaledHalfHeightAdjust > 0.0f)
				{
					// Shrink to a short capsule, sweep down to base to find where that would hit something, and then try to stand up from there.
					float PawnRadius, PawnHalfHeight;
					CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);
					const float ShrinkHalfHeight = PawnHalfHeight - PawnRadius;
					const float TraceDist = PawnHalfHeight - ShrinkHalfHeight;

					FHitResult Hit(1.0f);
					const FCollisionShape ShortCapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_HeightCustom, ShrinkHalfHeight);
					const bool bBlockingHit = GetWorld()->SweepSingleByChannel(Hit, PawnLocation, PawnLocation + CapsuleDown * TraceDist, PawnRotation, CollisionChannel, ShortCapsuleShape, CapsuleParams);
					if (Hit.bStartPenetrating)
					{
						bEncroached = true;
					}
					else
					{
						// Compute where the base of the sweep ended up, and see if we can stand there.
						const float DistanceToBase = (Hit.Time * TraceDist) + ShortCapsuleShape.Capsule.HalfHeight;
						const FVector NewLoc = PawnLocation - CapsuleDown * (-DistanceToBase + PawnHalfHeight + SweepInflation + MIN_FLOOR_DIST / 2.0f);
						bEncroached = GetWorld()->OverlapBlockingTestByChannel(NewLoc, PawnRotation, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);
						if (!bEncroached)
						{
							// Intentionally not using MoveUpdatedComponent, where a horizontal plane constraint would prevent the base of the capsule from staying at the same spot.
							UpdatedComponent->MoveComponent(NewLoc - PawnLocation, PawnRotation, false);
						}
					}
				}
			}
		}
		else
		{
			// Expand while keeping base location the same.
			FVector StandingLocation = PawnLocation - CapsuleDown * (StandingCapsuleShape.GetCapsuleHalfHeight() - CurrentCrouchedHalfHeight);
			bEncroached = GetWorld()->OverlapBlockingTestByChannel(StandingLocation, PawnRotation, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);

			if (bEncroached)
			{
				if (IsMovingOnGround())
				{
					// Something might be just barely overhead, try moving down closer to the floor to avoid it.
					const float MinFloorDist = KINDA_SMALL_NUMBER * 10.0f;
					if (CurrentFloor.bBlockingHit && CurrentFloor.FloorDist > MinFloorDist)
					{
						StandingLocation += CapsuleDown * (CurrentFloor.FloorDist - MinFloorDist);
						bEncroached = GetWorld()->OverlapBlockingTestByChannel(StandingLocation, PawnRotation, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);
					}
				}
			}

			if (!bEncroached)
			{
				// Commit the change in location.
				UpdatedComponent->MoveComponent(StandingLocation - PawnLocation, PawnRotation, false);
				bForceNextFloorCheck = true;
			}
		}

		// If still encroached then abort.
		if (bEncroached)
		{
			CharacterOwner->GetCapsuleComponent()->SetCapsuleSize(CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), OldUnscaledHalfHeight, false);
			return;
		}

		CharacterOwner->bIsCrouched = false;
	}
	else
	{
		bShrinkProxyCapsule = true;
	}

	// Now call SetCapsuleSize() to cause touch/untouch events.
	bUpdateOverlaps = true;
	CharacterOwner->GetCapsuleComponent()->SetCapsuleSize(DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), bUpdateOverlaps);

	const float MeshAdjust = ScaledHalfHeightAdjust;
	AdjustProxyCapsuleSize();
	CharacterOwner->OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);

	// Don't smooth this change in mesh position.
	if (bClientSimulation && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
		if (ClientData)
		{
			const FVector CapsuleUp = GetComponentAxisZ();
			const float MeshTranslationOffsetZ = ClientData->MeshTranslationOffset | CapsuleUp;
			if (MeshTranslationOffsetZ != 0.0f)
			{
				ClientData->MeshTranslationOffset += CapsuleUp * MeshAdjust;
				ClientData->OriginalMeshTranslationOffset = ClientData->MeshTranslationOffset;
			}
		}
	}
}

float UDashCharacterMovementComponent::SlideAlongSurface(const FVector& Delta, float Time, const FVector& InNormal, FHitResult& Hit, bool bHandleImpact)
{
	if (!Hit.bBlockingHit)
	{
		return 0.0f;
	}

	FVector NewNormal(InNormal);
	if (IsMovingOnGround())
	{
		const FVector CapsuleUp = GetComponentAxisZ();
		const float Dot = NewNormal | CapsuleUp;

		// We don't want to be pushed up an unwalkable surface.
		if (Dot > 0.0f)
		{
			if (!IsWalkable(Hit))
			{
				NewNormal = FVector::VectorPlaneProject(NewNormal, CapsuleUp).GetSafeNormal();
			}
		}
		else if (Dot < -KINDA_SMALL_NUMBER)
		{
			// Don't push down into the floor when the impact is on the upper portion of the capsule.
			if (CurrentFloor.FloorDist < MIN_FLOOR_DIST && CurrentFloor.bBlockingHit)
			{
				const FVector FloorNormal = CurrentFloor.HitResult.Normal;
				const bool bFloorOpposedToMovement = (Delta | FloorNormal) < 0.0f && (FloorNormal | CapsuleUp) < 1.0f - DELTA;
				if (bFloorOpposedToMovement)
				{
					NewNormal = FloorNormal;
				}

				NewNormal = FVector::VectorPlaneProject(NewNormal, CapsuleUp).GetSafeNormal();
			}
		}
	}

	return UPawnMovementComponent::SlideAlongSurface(Delta, Time, NewNormal, Hit, bHandleImpact);
}

void UDashCharacterMovementComponent::TwoWallAdjust(FVector& Delta, const FHitResult& Hit, const FVector& OldHitNormal) const
{
	const FVector InDelta = Delta;
	UPawnMovementComponent::TwoWallAdjust(Delta, Hit, OldHitNormal);

	if (IsMovingOnGround())
	{
		const FVector CapsuleUp = GetComponentAxisZ();
		const float DotDelta = Delta | CapsuleUp;

		// Allow slides up walkable surfaces, but not unwalkable ones (treat those as vertical barriers).
		if (DotDelta > 0.0f)
		{
			const float DotHitNormal = Hit.Normal | CapsuleUp;

			if (DotHitNormal > KINDA_SMALL_NUMBER && (DotHitNormal >= GetWalkableFloorZ() || IsWalkable(Hit)))
			{
				// Maintain horizontal velocity.
				const float Time = (1.0f - Hit.Time);
				const FVector ScaledDelta = Delta.GetSafeNormal() * InDelta.Size();
				Delta = (FVector::VectorPlaneProject(InDelta, CapsuleUp) + CapsuleUp * ((ScaledDelta | CapsuleUp) / DotHitNormal)) * Time;
			}
			else
			{
				Delta = FVector::VectorPlaneProject(Delta, CapsuleUp);
			}
		}
		else if (DotDelta < 0.0f)
		{
			// Don't push down into the floor.
			if (CurrentFloor.FloorDist < MIN_FLOOR_DIST && CurrentFloor.bBlockingHit)
			{
				Delta = FVector::VectorPlaneProject(Delta, CapsuleUp);
			}
		}
	}
}

FVector UDashCharacterMovementComponent::HandleSlopeBoosting(const FVector& SlideResult, const FVector& Delta, const float Time, const FVector& Normal, const FHitResult& Hit) const
{
	const FVector CapsuleUp = GetComponentAxisZ();
	FVector Result = SlideResult;
	const float Dot = Result | CapsuleUp;

	// Prevent boosting up slopes.
	if (Dot > 0.0f)
	{
		// Don't move any higher than we originally intended.
		const float ZLimit = (Delta | CapsuleUp) * Time;
		if (Dot - ZLimit > KINDA_SMALL_NUMBER)
		{
			if (ZLimit > 0.0f)
			{
				// Rescale the entire vector (not just the Z component) otherwise we change the direction and likely head right back into the impact.
				const float UpPercent = ZLimit / Dot;
				Result *= UpPercent;
			}
			else
			{
				// We were heading down but were going to deflect upwards. Just make the deflection horizontal.
				Result = FVector::ZeroVector;
			}

			// Make remaining portion of original result horizontal and parallel to impact normal.
			const FVector RemainderXY = FVector::VectorPlaneProject(SlideResult - Result, CapsuleUp);
			const FVector NormalXY = FVector::VectorPlaneProject(Normal, CapsuleUp).GetSafeNormal();
			const FVector Adjust = UPawnMovementComponent::ComputeSlideVector(RemainderXY, 1.0f, NormalXY, Hit);
			Result += Adjust;
		}
	}

	return Result;
}

float UDashCharacterMovementComponent::ImmersionDepth() const
{
	float Depth = 0.0f;

	if (CharacterOwner && GetPhysicsVolume()->bWaterVolume)
	{
		const float CollisionHalfHeight = CharacterOwner->GetSimpleCollisionHalfHeight();

		if (CollisionHalfHeight == 0.0f || Buoyancy == 0.0f)
		{
			Depth = 1.0f;
		}
		else
		{
			UBrushComponent* VolumeBrushComp = GetPhysicsVolume()->GetBrushComponent();
			FHitResult Hit(1.0f);
			if (VolumeBrushComp)
			{
				const FVector CapsuleHalfHeight = GetComponentAxisZ() * CollisionHalfHeight;
				const FVector TraceStart = UpdatedComponent->GetComponentLocation() + CapsuleHalfHeight;
				const FVector TraceEnd = UpdatedComponent->GetComponentLocation() - CapsuleHalfHeight;

				FCollisionQueryParams NewTraceParams(DashCharacterMovementComponentStatics::ImmersionDepthName, true);
				VolumeBrushComp->LineTraceComponent(Hit, TraceStart, TraceEnd, NewTraceParams);
			}

			Depth = (Hit.Time == 1.0f) ? 1.0f : (1.0f - Hit.Time);
		}
	}

	return Depth;
}

void UDashCharacterMovementComponent::RequestDirectMove(const FVector& MoveVelocity, bool bForceMaxSpeed)
{
	if (MoveVelocity.SizeSquared() < KINDA_SMALL_NUMBER)
	{
		return;
	}

	if (IsFalling())
	{
		const FVector FallVelocity = MoveVelocity.GetClampedToMaxSize(GetMaxSpeed());
		const FVector GravityDir = GetGravityDirection();
		if (!GravityDir.IsZero())
		{
			PerformAirControlForPathFollowingEx(FallVelocity, GravityDir);
		}

		return;
	}

	RequestedVelocity = MoveVelocity;
	bHasRequestedVelocity = true;
	bRequestedMoveWithMaxSpeed = bForceMaxSpeed;

	if (IsMovingOnGround())
	{
		RequestedVelocity = FVector::VectorPlaneProject(RequestedVelocity, GetComponentAxisZ());
	}
}

float UDashCharacterMovementComponent::GetMaxJumpHeight() const
{
	const float GravityMagnitude = GetGravityMagnitude();
	if (GravityMagnitude > KINDA_SMALL_NUMBER)
	{
		return FMath::Square(JumpZVelocity) / (2.0f * GravityMagnitude);
	}
	else
	{
		return 0.0f;
	}
}

void UDashCharacterMovementComponent::PhysFlying(float deltaTime, int32 Iterations)
{
	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	// Abort if no valid gravity can be obtained.
	const FVector GravDir = GetGravityDirection();
	if (GravDir.IsZero())
	{
		Acceleration = FVector::ZeroVector;
		Velocity = FVector::ZeroVector;
		return;
	}

	RestorePreAdditiveRootMotionVelocity();

	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
	{
		if (bCheatFlying && Acceleration.IsZero())
		{
			Velocity = FVector::ZeroVector;
		}
		const float Friction = 0.5f * GetPhysicsVolume()->FluidFriction;
		CalcVelocity(deltaTime, Friction, true, BrakingDecelerationFlying);
	}

	ApplyRootMotionToVelocityOVERRIDEN(deltaTime);

	Iterations++;
	bJustTeleported = false;

	FVector OldLocation = UpdatedComponent->GetComponentLocation();
	const FVector Adjusted = Velocity * deltaTime;
	FHitResult Hit(1.0f);
	SafeMoveUpdatedComponent(Adjusted, UpdatedComponent->GetComponentQuat(), true, Hit);

	if (Hit.Time < 1.0f)
	{
		const float UpDown = GravDir | Velocity.GetSafeNormal();
		bool bSteppedUp = false;

		if (UpDown < 0.5f && UpDown > -0.2f && FMath::Abs(Hit.ImpactNormal | GravDir) < 0.2f && CanStepUp(Hit))
		{
			const FVector StepLocation = UpdatedComponent->GetComponentLocation();

			bSteppedUp = StepUp(GravDir, Adjusted * (1.0f - Hit.Time), Hit);
			if (bSteppedUp)
			{
				OldLocation += GravDir * ((UpdatedComponent->GetComponentLocation() - StepLocation) | GravDir);
			}
		}

		if (!bSteppedUp)
		{
			// Adjust and try again.
			HandleImpact(Hit, deltaTime, Adjusted);
			SlideAlongSurface(Adjusted, (1.0f - Hit.Time), Hit.Normal, Hit, true);
		}
	}

	if (!bJustTeleported && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
	{
		Velocity = (UpdatedComponent->GetComponentLocation() - OldLocation) / deltaTime;
	}
}

void UDashCharacterMovementComponent::ApplyRootMotionToVelocityOVERRIDEN(float deltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_CharacterMovementRootMotionSourceApply);

	// Animation root motion is distinct from root motion sources right now and takes precedence.
	if (HasAnimRootMotion() && deltaTime > 0.0f)
	{
		Velocity = ConstrainAnimRootMotionVelocity(AnimRootMotionVelocity, Velocity);
		return;
	}

	const FVector OldVelocity = Velocity;

	bool bAppliedRootMotion = false;

	// Apply override velocity.
	if (CurrentRootMotion.HasOverrideVelocity())
	{
		CurrentRootMotion.AccumulateOverrideRootMotionVelocity(deltaTime, *CharacterOwner, *this, Velocity);
		bAppliedRootMotion = true;
	}

	// Next apply additive root motion.
	if (CurrentRootMotion.HasAdditiveVelocity())
	{
		CurrentRootMotion.LastPreAdditiveVelocity = Velocity; // Save off pre-additive velocity for restoration next tick.
		CurrentRootMotion.AccumulateAdditiveRootMotionVelocity(deltaTime, *CharacterOwner, *this, Velocity);
		CurrentRootMotion.bIsAdditiveVelocityApplied = true; // Remember that we have it applied.
		bAppliedRootMotion = true;
	}

	// Switch to falling if we have vertical velocity from root motion so we can lift off the ground.
	if (bAppliedRootMotion && IsMovingOnGround())
	{
		const float AppliedVelocityDeltaZ = (Velocity - OldVelocity) | GetComponentAxisZ();

		if (AppliedVelocityDeltaZ > 0.0f)
		{
			float LiftoffBound;
			if (CurrentRootMotion.LastAccumulatedSettings.HasFlag(ERootMotionSourceSettingsFlags::UseSensitiveLiftoffCheck))
			{
				// Sensitive bounds - "any positive force".
				LiftoffBound = SMALL_NUMBER;
			}
			else
			{
				// Default bounds - the amount of force gravity is applying this tick.
				LiftoffBound = FMath::Max(GetGravityMagnitude() * deltaTime, SMALL_NUMBER);
			}

			if (AppliedVelocityDeltaZ > LiftoffBound)
			{
				SetMovementMode(MOVE_Falling);
			}
		}
	}
}

void UDashCharacterMovementComponent::PhysSwimming(float deltaTime, int32 Iterations)
{
	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	// Abort if no valid gravity can be obtained.
	const FVector GravityDir = GetGravityDirection();
	if (GravityDir.IsZero())
	{
		Acceleration = FVector::ZeroVector;
		Velocity = FVector::ZeroVector;
		return;
	}

	RestorePreAdditiveRootMotionVelocity();

	float VelocityZ = (Velocity | GravityDir) * -1.0f;
	const float AccelerationZ = (Acceleration | GravityDir) * -1.0f;
	const float Depth = ImmersionDepth();
	const float NetBuoyancy = Buoyancy * Depth;
	const float OriginalAccelZ = AccelerationZ;
	bool bLimitedUpAccel = false;

	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && VelocityZ > 0.33f * MaxSwimSpeed && NetBuoyancy != 0.0f)
	{
		// Damp velocity out of water.
		Velocity = FVector::VectorPlaneProject(Velocity, GravityDir) + GravityDir * (FMath::Max(0.33f * MaxSwimSpeed, VelocityZ * Depth * Depth) * -1.0f);
	}
	else if (Depth < 0.65f)
	{
		bLimitedUpAccel = (AccelerationZ > 0.0f);
		Acceleration = FVector::VectorPlaneProject(Acceleration, GravityDir) + GravityDir * (FMath::Min(0.1f, AccelerationZ) * -1.0f);
	}

	Iterations++;
	FVector OldLocation = UpdatedComponent->GetComponentLocation();
	bJustTeleported = false;

	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
	{
		const float Friction = 0.5f * GetPhysicsVolume()->FluidFriction * Depth;
		CalcVelocity(deltaTime, Friction, true, BrakingDecelerationSwimming);
		Velocity += GetGravity() * (deltaTime * (1.0f - NetBuoyancy));
	}

	ApplyRootMotionToVelocityOVERRIDEN(deltaTime);

	FVector Adjusted = Velocity * deltaTime;
	FHitResult Hit(1.0f);
	const float remainingTime = deltaTime * Swim(Adjusted, Hit);

	// May have left water - if so, script might have set new physics mode.
	if (!IsSwimming())
	{
		StartNewPhysics(remainingTime, Iterations);
		return;
	}

	if (Hit.Time < 1.0f && CharacterOwner)
	{
		VelocityZ = (Velocity | GravityDir) * -1.0f;
		if (bLimitedUpAccel && VelocityZ >= 0.0f)
		{
			// Allow upward velocity at surface if against obstacle.
			Velocity = FVector::VectorPlaneProject(Velocity, GravityDir) + GravityDir * ((VelocityZ + OriginalAccelZ * deltaTime) * -1.0f);
			Adjusted = Velocity * (1.0f - Hit.Time) * deltaTime;
			Swim(Adjusted, Hit);
			if (!IsSwimming())
			{
				StartNewPhysics(remainingTime, Iterations);
				return;
			}
		}

		const float UpDown = GravityDir | Velocity.GetSafeNormal();
		bool bSteppedUp = false;

		if (UpDown < 0.5f && UpDown > -0.2f && FMath::Abs(Hit.ImpactNormal | GravityDir) < 0.2f && CanStepUp(Hit))
		{
			const FVector StepLocation = UpdatedComponent->GetComponentLocation();
			const FVector RealVelocity = Velocity;
			Velocity = FVector::VectorPlaneProject(Velocity, GravityDir) - GravityDir; // HACK: since will be moving up, in case pawn leaves the water.

			bSteppedUp = StepUp(GravityDir, Adjusted * (1.0f - Hit.Time), Hit);
			if (bSteppedUp)
			{
				// May have left water; if so, script might have set new physics mode.
				if (!IsSwimming())
				{
					StartNewPhysics(remainingTime, Iterations);
					return;
				}

				OldLocation += GravityDir * ((UpdatedComponent->GetComponentLocation() - StepLocation) | GravityDir);
			}

			Velocity = RealVelocity;
		}

		if (!bSteppedUp)
		{
			// Adjust and try again.
			HandleImpact(Hit, deltaTime, Adjusted);
			SlideAlongSurface(Adjusted, 1.0f - Hit.Time, Hit.Normal, Hit, true);
		}
	}

	if (CharacterOwner && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && !bJustTeleported && (deltaTime - remainingTime) > KINDA_SMALL_NUMBER)
	{
		const float VelZ = Velocity | GravityDir;
		Velocity = (UpdatedComponent->GetComponentLocation() - OldLocation) / (deltaTime - remainingTime);

		if (!GetPhysicsVolume()->bWaterVolume)
		{
			Velocity = FVector::VectorPlaneProject(Velocity, GravityDir) + GravityDir * VelZ;
		}
	}

	if (!GetPhysicsVolume()->bWaterVolume && IsSwimming())
	{
		SetMovementMode(MOVE_Falling); // In case script didn't change it (w/ zone change).
	}

	// May have left water - if so, script might have set new physics mode.
	if (!IsSwimming())
	{
		StartNewPhysics(remainingTime, Iterations);
	}
}

void UDashCharacterMovementComponent::StartSwimmingOVERRIDEN(FVector OldLocation, FVector OldVelocity, float timeTick, float remainingTime, int32 Iterations)
{
	if (remainingTime < MIN_TICK_TIME || timeTick < MIN_TICK_TIME)
	{
		return;
	}

	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && !bJustTeleported)
	{
		Velocity = (UpdatedComponent->GetComponentLocation() - OldLocation) / timeTick; // Actual average velocity.
		Velocity = 2.0f * Velocity - OldVelocity; // End velocity has 2x accel of avg.
		Velocity = Velocity.GetClampedToMaxSize(GetPhysicsVolume()->TerminalVelocity);
	}

	const FVector End = FindWaterLine(UpdatedComponent->GetComponentLocation(), OldLocation);
	float waterTime = 0.0f;
	if (End != UpdatedComponent->GetComponentLocation())
	{
		const float ActualDist = (UpdatedComponent->GetComponentLocation() - OldLocation).Size();
		if (ActualDist > KINDA_SMALL_NUMBER)
		{
			waterTime = timeTick * (End - UpdatedComponent->GetComponentLocation()).Size() / ActualDist;
			remainingTime += waterTime;
		}

		MoveUpdatedComponent(End - UpdatedComponent->GetComponentLocation(), UpdatedComponent->GetComponentQuat(), true);
	}

	const FVector GravityDir = GetGravityDirection();
	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && !GravityDir.IsZero())
	{
		const float Dot = Velocity | GravityDir;
		if (Dot > 0.0f && Dot < SWIMBOBSPEED * -2.0f)
		{
			// Apply smooth bobbing.
			const FVector Velocity2D = FVector::VectorPlaneProject(Velocity, GravityDir);
			Velocity = Velocity2D + GravityDir * ((SWIMBOBSPEED - Velocity2D.Size() * 0.7f) * -1.0f);
		}
	}

	if (remainingTime >= MIN_TICK_TIME && Iterations < MaxSimulationIterations)
	{
		PhysSwimming(remainingTime, Iterations);
	}
}

FVector UDashCharacterMovementComponent::GetFallingLateralAcceleration(float DeltaTime)
{
	return GetFallingLateralAccelerationEx(DeltaTime, GetGravityDirection(true));
}

FVector UDashCharacterMovementComponent::GetFallingLateralAccelerationEx(float DeltaTime, const FVector& GravDir) const
{
	// No vertical acceleration.
	FVector FallAcceleration = FVector::VectorPlaneProject(Acceleration, GravDir);

	// Bound acceleration, falling object has minimal ability to impact acceleration.
	if (!HasAnimRootMotion() && FallAcceleration.SizeSquared() > 0.0f)
	{
		FallAcceleration = GetAirControlEx(DeltaTime, AirControl, FallAcceleration, GravDir);
		FallAcceleration = FallAcceleration.GetClampedToMaxSize(GetMaxAcceleration());
	}

	return FallAcceleration;
}

FVector UDashCharacterMovementComponent::GetAirControl(float DeltaTime, float TickAirControl, const FVector& FallAcceleration)
{
	return GetAirControlEx(DeltaTime, TickAirControl, FallAcceleration, GetGravityDirection(true));
}

FVector UDashCharacterMovementComponent::GetAirControlEx(float DeltaTime, float TickAirControl, const FVector& FallAcceleration, const FVector& GravDir) const
{
	// Boost.
	if (TickAirControl != 0.0f)
	{
		TickAirControl = BoostAirControlEx(DeltaTime, TickAirControl, FallAcceleration, GravDir);
	}

	return TickAirControl * FallAcceleration;
}

float UDashCharacterMovementComponent::BoostAirControl(float DeltaTime, float TickAirControl, const FVector& FallAcceleration)
{
	return BoostAirControlEx(DeltaTime, TickAirControl, FallAcceleration, GetGravityDirection(true));
}

float UDashCharacterMovementComponent::BoostAirControlEx(float DeltaTime, float TickAirControl, const FVector& FallAcceleration, const FVector& GravDir) const
{
	// Allow a burst of initial acceleration.
	if (AirControlBoostMultiplier > 0.0f && FVector::VectorPlaneProject(Velocity, GravDir).SizeSquared() < FMath::Square(AirControlBoostVelocityThreshold))
	{
		TickAirControl = FMath::Min(1.0f, AirControlBoostMultiplier * TickAirControl);
	}

	return TickAirControl;
}

void UDashCharacterMovementComponent::PhysFalling(float deltaTime, int32 Iterations)
{
	SCOPE_CYCLE_COUNTER(STAT_CharPhysFalling);

	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	// Abort if no valid gravity can be obtained.
	const FVector GravityDir = GetGravityDirection();
	if (GravityDir.IsZero())
	{
		Acceleration = FVector::ZeroVector;
		Velocity = FVector::ZeroVector;
		return;
	}

	FVector FallAcceleration = GetFallingLateralAccelerationEx(deltaTime, GravityDir);
	const bool bHasAirControl = FallAcceleration.SizeSquared() > 0.0f;

	float RemainingTime = deltaTime;
	while (RemainingTime >= MIN_TICK_TIME && Iterations < MaxSimulationIterations)
	{
		Iterations++;
		const float timeTick = GetSimulationTimeStep(RemainingTime, Iterations);
		RemainingTime -= timeTick;

		const FVector OldLocation = UpdatedComponent->GetComponentLocation();
		const FQuat PawnRotation = UpdatedComponent->GetComponentQuat();
		bJustTeleported = false;

		RestorePreAdditiveRootMotionVelocity();

		FVector OldVelocity = Velocity;
		FVector VelocityNoAirControl = Velocity;

		// Apply input.
		if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
		{
			const FVector OldVelocityZ = GravityDir * (Velocity | GravityDir);

			// Compute VelocityNoAirControl.
			if (bHasAirControl)
			{
				// Find velocity *without* acceleration.
				TGuardValue<FVector> RestoreAcceleration(Acceleration, FVector::ZeroVector);
				TGuardValue<FVector> RestoreVelocity(Velocity, Velocity);

				Velocity = FVector::VectorPlaneProject(Velocity, GravityDir);
				CalcVelocity(timeTick, FallingLateralFriction, false, BrakingDecelerationFalling);
				VelocityNoAirControl = FVector::VectorPlaneProject(Velocity, GravityDir) + OldVelocityZ;
			}

			// Compute Velocity.
			{
				// Acceleration = FallAcceleration for CalcVelocity(), but we restore it after using it.
				TGuardValue<FVector> RestoreAcceleration(Acceleration, FallAcceleration);

				Velocity = FVector::VectorPlaneProject(Velocity, GravityDir);
				CalcVelocity(timeTick, FallingLateralFriction, false, BrakingDecelerationFalling);
				Velocity = FVector::VectorPlaneProject(Velocity, GravityDir) + OldVelocityZ;
			}

			// Just copy Velocity to VelocityNoAirControl if they are the same (ie no acceleration).
			if (!bHasAirControl)
			{
				VelocityNoAirControl = Velocity;
			}
		}

		// Apply gravity.
		const FVector Gravity = GetGravity();
		Velocity = NewFallVelocity(Velocity, Gravity, timeTick);
		VelocityNoAirControl = NewFallVelocity(VelocityNoAirControl, Gravity, timeTick);
		const FVector AirControlAccel = (Velocity - VelocityNoAirControl) / timeTick;

		ApplyRootMotionToVelocityOVERRIDEN(timeTick);

		if (bNotifyApex && CharacterOwner->Controller && ((Velocity | GravityDir) * -1.0f) <= 0.0f)
		{
			// Just passed jump apex since now going down.
			bNotifyApex = false;
			NotifyJumpApex();
		}

		// Move now.
		FHitResult Hit(1.0f);
		FVector Adjusted = 0.5f * (OldVelocity + Velocity) * timeTick;
		SafeMoveUpdatedComponent(Adjusted, PawnRotation, true, Hit);

		if (!HasValidData())
		{
			return;
		}

		float LastMoveTimeSlice = timeTick;
		float SubTimeTickRemaining = timeTick * (1.0f - Hit.Time);

		if (IsSwimming())
		{
			// Just entered water.
			RemainingTime += SubTimeTickRemaining;
			StartSwimmingOVERRIDEN(OldLocation, OldVelocity, timeTick, RemainingTime, Iterations);
			return;
		}
		else if (Hit.bBlockingHit)
		{
			if (IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit))
			{
				RemainingTime += SubTimeTickRemaining;
				ProcessLanded(Hit, RemainingTime, Iterations);
				return;
			}
			else
			{
				// Compute impact deflection based on final velocity, not integration step.
				// This allows us to compute a new velocity from the deflected vector, and ensures the full gravity effect is included in the slide result.
				Adjusted = Velocity * timeTick;

				// See if we can convert a normally invalid landing spot (based on the hit result) to a usable one.
				if (!Hit.bStartPenetrating && ShouldCheckForValidLandingSpot(timeTick, Adjusted, Hit))
				{
					const FVector PawnLocation = UpdatedComponent->GetComponentLocation();
					FFindFloorResult FloorResult;
					FindFloor(PawnLocation, FloorResult, false);
					if (FloorResult.IsWalkableFloor() && IsValidLandingSpot(PawnLocation, FloorResult.HitResult))
					{
						RemainingTime += SubTimeTickRemaining;
						ProcessLanded(FloorResult.HitResult, RemainingTime, Iterations);
						return;
					}
				}

				HandleImpact(Hit, LastMoveTimeSlice, Adjusted);

				// If we've changed physics mode, abort.
				if (!HasValidData() || !IsFalling())
				{
					return;
				}

				// Limit air control based on what we hit.
				// We moved to the impact point using air control, but may want to deflect from there based on a limited air control acceleration.
				if (bHasAirControl)
				{
					const FVector AirControlDeltaV = LimitAirControlEx(LastMoveTimeSlice, AirControlAccel, Hit, GravityDir, false) * LastMoveTimeSlice;
					Adjusted = (VelocityNoAirControl + AirControlDeltaV) * LastMoveTimeSlice;
				}

				const FVector OldHitNormal = Hit.Normal;
				const FVector OldHitImpactNormal = Hit.ImpactNormal;
				FVector Delta = ComputeSlideVector(Adjusted, 1.0f - Hit.Time, OldHitNormal, Hit);

				// Compute velocity after deflection (only gravity component for RootMotion).
				if (SubTimeTickRemaining > KINDA_SMALL_NUMBER && !bJustTeleported)
				{
					const FVector NewVelocity = (Delta / SubTimeTickRemaining);

					if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
					{
						Velocity = NewVelocity;
					}
					else
					{
						Velocity = FVector::VectorPlaneProject(Velocity, GravityDir) + GravityDir * (NewVelocity | GravityDir);
					}
				}

				if (SubTimeTickRemaining > KINDA_SMALL_NUMBER && (Delta | Adjusted) > 0.0f)
				{
					// Move in deflected direction.
					SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);

					if (Hit.bBlockingHit)
					{
						// Hit second wall.
						LastMoveTimeSlice = SubTimeTickRemaining;
						SubTimeTickRemaining *= (1.0f - Hit.Time);

						if (IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit))
						{
							RemainingTime += SubTimeTickRemaining;
							ProcessLanded(Hit, RemainingTime, Iterations);
							return;
						}

						HandleImpact(Hit, LastMoveTimeSlice, Delta);

						// If we've changed physics mode, abort.
						if (!HasValidData() || !IsFalling())
						{
							return;
						}

						// Act as if there was no air control on the last move when computing new deflection.
						if (bHasAirControl && (Hit.Normal | GravityDir) < -VERTICAL_SLOPE_NORMAL_Z)
						{
							Delta = ComputeSlideVector(VelocityNoAirControl * LastMoveTimeSlice, 1.0f, OldHitNormal, Hit);
						}

						FVector PreTwoWallDelta = Delta;
						TwoWallAdjust(Delta, Hit, OldHitNormal);

						// Limit air control, but allow a slide along the second wall.
						if (bHasAirControl)
						{
							const FVector AirControlDeltaV = LimitAirControlEx(SubTimeTickRemaining, AirControlAccel, Hit, GravityDir, false) * SubTimeTickRemaining;

							// Only allow if not back in to first wall.
							if ((AirControlDeltaV | OldHitNormal) > 0.0f)
							{
								Delta += (AirControlDeltaV * SubTimeTickRemaining);
							}
						}

						// Compute velocity after deflection (only gravity component for RootMotion).
						if (SubTimeTickRemaining > KINDA_SMALL_NUMBER && !bJustTeleported)
						{
							const FVector NewVelocity = (Delta / SubTimeTickRemaining);

							if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
							{
								Velocity = NewVelocity;
							}
							else
							{
								Velocity = FVector::VectorPlaneProject(Velocity, GravityDir) + GravityDir * (NewVelocity | GravityDir);
							}
						}

						// bDitch=true means that pawn is straddling two slopes, neither of which he can stand on.
						bool bDitch = ((OldHitImpactNormal | GravityDir) < 0.0f && (Hit.ImpactNormal | GravityDir) < 0.0f &&
							FMath::Abs(Delta | GravityDir) <= KINDA_SMALL_NUMBER && (Hit.ImpactNormal | OldHitImpactNormal) < 0.0f);

						SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);

						if (Hit.Time == 0.0f)
						{
							// If we are stuck then try to side step.
							FVector SideDelta = FVector::VectorPlaneProject(OldHitNormal + Hit.ImpactNormal, GravityDir).GetSafeNormal();
							if (SideDelta.IsNearlyZero())
							{
								SideDelta = GravityDir ^ (FVector::VectorPlaneProject(OldHitNormal, GravityDir).GetSafeNormal());
							}

							SafeMoveUpdatedComponent(SideDelta, PawnRotation, true, Hit);
						}

						if (bDitch || IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit) || Hit.Time == 0.0f)
						{
							RemainingTime = 0.0f;
							ProcessLanded(Hit, RemainingTime, Iterations);

							return;
						}
						else if (GetPerchRadiusThreshold() > 0.0f && Hit.Time == 1.0f && (OldHitImpactNormal | GravityDir) <= -GetWalkableFloorZ())
						{
							// We might be in a virtual 'ditch' within our perch radius. This is rare.
							const FVector PawnLocation = UpdatedComponent->GetComponentLocation();
							const float ZMovedDist = FMath::Abs((PawnLocation - OldLocation) | GravityDir);
							const float MovedDist2DSq = (FVector::VectorPlaneProject(PawnLocation - OldLocation, GravityDir)).SizeSquared();

							if (ZMovedDist <= 0.2f * timeTick && MovedDist2DSq <= 4.0f * timeTick)
							{
								Velocity.X += 0.25f * GetMaxSpeed() * (FMath::FRand() - 0.5f);
								Velocity.Y += 0.25f * GetMaxSpeed() * (FMath::FRand() - 0.5f);
								Velocity.Z += 0.25f * GetMaxSpeed() * (FMath::FRand() - 0.5f);
								Velocity = FVector::VectorPlaneProject(Velocity, GravityDir) + GravityDir * (FMath::Max<float>(JumpZVelocity * 0.25f, 1.0f) * -1.0f);
								Delta = Velocity * timeTick;

								SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);
							}
						}
					}
				}
			}
		}

		if ((FVector::VectorPlaneProject(Velocity, GravityDir)).SizeSquared() <= KINDA_SMALL_NUMBER * 10.0f)
		{
			Velocity = GravityDir * (Velocity | GravityDir);
		}
	}
}

FVector UDashCharacterMovementComponent::LimitAirControl(float DeltaTime, const FVector& FallAcceleration, const FHitResult& HitResult, bool bCheckForValidLandingSpot)
{
	return LimitAirControlEx(DeltaTime, FallAcceleration, HitResult, GetGravityDirection(true), bCheckForValidLandingSpot);
}

FVector UDashCharacterMovementComponent::LimitAirControlEx(float DeltaTime, const FVector& FallAcceleration, const FHitResult& HitResult, const FVector& GravDir, bool bCheckForValidLandingSpot) const
{
	FVector Result = FallAcceleration;

	if (HitResult.IsValidBlockingHit() && (HitResult.Normal | GravDir) < -VERTICAL_SLOPE_NORMAL_Z)
	{
		if ((!bCheckForValidLandingSpot || !IsValidLandingSpot(HitResult.Location, HitResult)) && (FallAcceleration | HitResult.Normal) < 0.0f)
		{
			// If acceleration is into the wall, limit contribution.
			// Allow movement parallel to the wall, but not into it because that may push us up.
			const FVector Normal2D = FVector::VectorPlaneProject(HitResult.Normal, GravDir).GetSafeNormal();
			Result = FVector::VectorPlaneProject(FallAcceleration, Normal2D);
		}
	}
	else if (HitResult.bStartPenetrating)
	{
		// Allow movement out of penetration.
		return ((Result | HitResult.Normal) > 0.0f ? Result : FVector::ZeroVector);
	}

	return Result;
}

bool UDashCharacterMovementComponent::CheckLedgeDirection(const FVector& OldLocation, const FVector& SideStep, const FVector& GravDir) const
{
	const FVector SideDest = OldLocation + SideStep;
	const FQuat PawnRotation = UpdatedComponent->GetComponentQuat();
	FCollisionQueryParams CapsuleParams(DashCharacterMovementComponentStatics::CheckLedgeDirectionName, false, CharacterOwner);
	FCollisionResponseParams ResponseParam;
	InitCollisionParams(CapsuleParams, ResponseParam);
	const FCollisionShape CapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_None);
	const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();
	FHitResult Result(1.0f);
	GetWorld()->SweepSingleByChannel(Result, OldLocation, SideDest, PawnRotation, CollisionChannel, CapsuleShape, CapsuleParams, ResponseParam);

	if (!Result.bBlockingHit || IsWalkable(Result))
	{
		if (!Result.bBlockingHit)
		{
			GetWorld()->SweepSingleByChannel(Result, SideDest, SideDest + GravDir * (MaxStepHeight + LedgeCheckThreshold), PawnRotation, CollisionChannel, CapsuleShape, CapsuleParams, ResponseParam);
		}

		if (Result.Time < 1.0f && IsWalkable(Result))
		{
			return true;
		}
	}

	return false;
}

FVector UDashCharacterMovementComponent::GetLedgeMove(const FVector& OldLocation, const FVector& Delta, const FVector& GravDir) const
{
	if (!HasValidData() || Delta.IsZero())
	{
		return FVector::ZeroVector;
	}

	FVector SideDir = FVector::VectorPlaneProject(Delta, GravDir);

	// Try left.
	SideDir = FQuat(GravDir, PI * 0.5f).RotateVector(SideDir);
	if (CheckLedgeDirection(OldLocation, SideDir, GravDir))
	{
		return SideDir;
	}

	// Try right.
	SideDir *= -1.0f;
	if (CheckLedgeDirection(OldLocation, SideDir, GravDir))
	{
		return SideDir;
	}

	return FVector::ZeroVector;
}

void UDashCharacterMovementComponent::StartFalling(int32 Iterations, float remainingTime, float timeTick, const FVector& Delta, const FVector& subLoc)
{
	const float DesiredDist = Delta.Size();

	if (DesiredDist < KINDA_SMALL_NUMBER)
	{
		remainingTime = 0.0f;
	}
	else
	{
		const float ActualDist = (UpdatedComponent->GetComponentLocation() - subLoc).Size();
		remainingTime += timeTick * (1.0f - FMath::Min(1.0f, ActualDist / DesiredDist));
	}

	if (IsMovingOnGround())
	{
		// This is to catch cases where the first frame of PIE is executed, and the
		// level is not yet visible. In those cases, the player will fall out of the
		// world... So, don't set MOVE_Falling straight away.
		if (!GIsEditor || (GetWorld()->HasBegunPlay() && GetWorld()->GetTimeSeconds() >= 1.0f))
		{
			SetMovementMode(MOVE_Falling); // Default behavior if script didn't change physics.
		}
		else
		{
			// Make sure that the floor check code continues processing during this delay.
			bForceNextFloorCheck = true;
		}
	}

	StartNewPhysics(remainingTime, Iterations);
}

FVector UDashCharacterMovementComponent::ComputeGroundMovementDelta(const FVector& Delta, const FHitResult& RampHit, const bool bHitFromLineTrace) const
{
	const FVector CapsuleUp = GetComponentAxisZ();
	return ComputeGroundMovementDeltaEx(FVector::VectorPlaneProject(Delta, CapsuleUp), CapsuleUp, RampHit, bHitFromLineTrace);
}

FVector UDashCharacterMovementComponent::ComputeGroundMovementDeltaEx(const FVector& Delta, const FVector& DeltaPlaneNormal, const FHitResult& RampHit, const bool bHitFromLineTrace) const
{
	const FVector FloorNormal = RampHit.ImpactNormal;

	if (!bHitFromLineTrace && FMath::Abs(Delta | FloorNormal) > THRESH_NORMALS_ARE_ORTHOGONAL && IsWalkable(RampHit))
	{
		// Compute a vector that moves parallel to the surface, by projecting the horizontal movement direction onto the ramp.
		// We can't just project Delta onto the plane defined by FloorNormal because the direction changes on spherical geometry.
		const FVector DeltaNormal = Delta.GetSafeNormal();
		FVector NewDelta = FQuat(DeltaPlaneNormal ^ DeltaNormal, FMath::Acos(FloorNormal | DeltaPlaneNormal)).RotateVector(Delta);

		if (bMaintainHorizontalGroundVelocity)
		{
			const FVector NewDeltaNormal = NewDelta.GetSafeNormal();
			NewDelta = NewDeltaNormal * (Delta.Size() / (DeltaNormal | NewDeltaNormal));
		}

		return NewDelta;
	}

	return Delta;
}

void UDashCharacterMovementComponent::MoveAlongFloor(const FVector& InVelocity, float DeltaSeconds, FStepDownResult* OutStepDownResult)
{
	if (!CurrentFloor.IsWalkableFloor())
	{
		return;
	}

	// Move along the current floor.
	const FVector CapsuleUp = GetComponentAxisZ();
	const FVector Delta = FVector::VectorPlaneProject(InVelocity, CapsuleUp) * DeltaSeconds;
	FHitResult Hit(1.0f);
	FVector RampVector = ComputeGroundMovementDeltaEx(Delta, CapsuleUp, CurrentFloor.HitResult, CurrentFloor.bLineTrace);
	SafeMoveUpdatedComponent(RampVector, UpdatedComponent->GetComponentQuat(), true, Hit);
	float LastMoveTimeSlice = DeltaSeconds;

	if (Hit.bStartPenetrating)
	{
		// Allow this hit to be used as an impact we can deflect off, otherwise we do nothing the rest of the update and appear to hitch.
		HandleImpact(Hit);
		SlideAlongSurface(Delta, 1.0f, Hit.Normal, Hit, true);

		if (Hit.bStartPenetrating)
		{
			OnCharacterStuckInGeometry(&Hit);
		}
	}
	else if (Hit.IsValidBlockingHit())
	{
		// We impacted something (most likely another ramp, but possibly a barrier).
		float PercentTimeApplied = Hit.Time;
		if (Hit.Time > 0.0f && (Hit.Normal | CapsuleUp) > KINDA_SMALL_NUMBER && IsWalkable(Hit))
		{
			// Another walkable ramp.
			const float InitialPercentRemaining = 1.0f - PercentTimeApplied;
			RampVector = ComputeGroundMovementDeltaEx(Delta * InitialPercentRemaining, CapsuleUp, Hit, false);
			LastMoveTimeSlice = InitialPercentRemaining * LastMoveTimeSlice;
			SafeMoveUpdatedComponent(RampVector, UpdatedComponent->GetComponentQuat(), true, Hit);

			const float SecondHitPercent = Hit.Time * InitialPercentRemaining;
			PercentTimeApplied = FMath::Clamp(PercentTimeApplied + SecondHitPercent, 0.0f, 1.0f);
		}

		if (Hit.IsValidBlockingHit())
		{
			if (CanStepUp(Hit) || (CharacterOwner->GetMovementBase() != NULL && CharacterOwner->GetMovementBase()->GetOwner() == Hit.GetActor()))
			{
				// Hit a barrier, try to step up.
				if (!StepUp(CapsuleUp * -1.0f, Delta * (1.0f - PercentTimeApplied), Hit, OutStepDownResult))
				{
					UE_LOG(LogCharacterMovement, Verbose, TEXT("- StepUp (ImpactNormal %s, Normal %s"), *Hit.ImpactNormal.ToString(), *Hit.Normal.ToString());
					HandleImpact(Hit, LastMoveTimeSlice, RampVector);
					SlideAlongSurface(Delta, 1.0f - PercentTimeApplied, Hit.Normal, Hit, true);
				}
				else
				{
					// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
					UE_LOG(LogCharacterMovement, Verbose, TEXT("+ StepUp (ImpactNormal %s, Normal %s"), *Hit.ImpactNormal.ToString(), *Hit.Normal.ToString());
					bJustTeleported |= !bMaintainHorizontalGroundVelocity;
				}
			}
			else if (Hit.Component.IsValid() && !Hit.Component.Get()->CanCharacterStepUp(CharacterOwner))
			{
				HandleImpact(Hit, LastMoveTimeSlice, RampVector);
				SlideAlongSurface(Delta, 1.0f - PercentTimeApplied, Hit.Normal, Hit, true);
			}
		}
	}
}

void UDashCharacterMovementComponent::MaintainHorizontalGroundVelocity()
{
	if (bMaintainHorizontalGroundVelocity)
	{
		// Just remove the vertical component.
		Velocity = FVector::VectorPlaneProject(Velocity, GetComponentAxisZ());
	}
	else
	{
		// Project the vector and maintain its original magnitude.
		Velocity = FVector::VectorPlaneProject(Velocity, GetComponentAxisZ()).GetSafeNormal() * Velocity.Size();
	}
}

void UDashCharacterMovementComponent::PhysWalking(float deltaTime, int32 Iterations)
{
	SCOPE_CYCLE_COUNTER(STAT_CharPhysWalking);

	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	if (!CharacterOwner || (!CharacterOwner->Controller && !bRunPhysicsWithNoController && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && CharacterOwner->GetLocalRole() != ROLE_SimulatedProxy))
	{
		Acceleration = FVector::ZeroVector;
		Velocity = FVector::ZeroVector;
		return;
	}

	if (!UpdatedComponent->IsQueryCollisionEnabled())
	{
		SetMovementMode(MOVE_Walking);
		return;
	}

	checkCode(ensureMsgf(!Velocity.ContainsNaN(), TEXT("PhysWalking: Velocity contains NaN before Iteration (%s)\n%s"), *GetPathNameSafe(this), *Velocity.ToString()));

	bJustTeleported = false;
	bool bCheckedFall = false;
	bool bTriedLedgeMove = false;
	float remainingTime = deltaTime;

	// Perform the move.
	while (remainingTime >= MIN_TICK_TIME && Iterations < MaxSimulationIterations && CharacterOwner && (CharacterOwner->Controller || bRunPhysicsWithNoController || HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity() || CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy))
	{
		Iterations++;
		bJustTeleported = false;
		const float timeTick = GetSimulationTimeStep(remainingTime, Iterations);
		remainingTime -= timeTick;

		// Save current values.
		UPrimitiveComponent* const OldBase = GetMovementBase();
		const FVector PreviousBaseLocation = (OldBase != NULL) ? OldBase->GetComponentLocation() : FVector::ZeroVector;
		const FVector OldLocation = UpdatedComponent->GetComponentLocation();
		const FFindFloorResult OldFloor = CurrentFloor;

		RestorePreAdditiveRootMotionVelocity();

		// Ensure velocity is horizontal.
		MaintainHorizontalGroundVelocity();

		const FVector OldVelocity = Velocity;
		Acceleration = FVector::VectorPlaneProject(Acceleration, GetComponentAxisZ());

		// Apply acceleration.
		if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
		{
			CalcVelocity(timeTick, GroundFriction, false, BrakingDecelerationWalking);
			checkCode(ensureMsgf(!Velocity.ContainsNaN(), TEXT("PhysWalking: Velocity contains NaN after CalcVelocity (%s)\n%s"), *GetPathNameSafe(this), *Velocity.ToString()));
		}

		ApplyRootMotionToVelocityOVERRIDEN(timeTick);
		checkCode(ensureMsgf(!Velocity.ContainsNaN(), TEXT("PhysWalking: Velocity contains NaN after Root Motion application (%s)\n%s"), *GetPathNameSafe(this), *Velocity.ToString()));

		if (IsFalling())
		{
			// Root motion could have put us into falling.
			// No movement has taken place this movement tick so we pass on full time/past iteration count.
			StartNewPhysics(remainingTime + timeTick, Iterations - 1);
			return;
		}

		// Compute move parameters.
		const FVector MoveVelocity = Velocity;
		const FVector Delta = timeTick * MoveVelocity;
		const bool bZeroDelta = Delta.IsNearlyZero();
		FStepDownResult StepDownResult;

		if (bZeroDelta)
		{
			remainingTime = 0.0f;
		}
		else
		{
			// Try to move forward.
			MoveAlongFloor(MoveVelocity, timeTick, &StepDownResult);

			if (IsFalling())
			{
				// Pawn decided to jump up.
				const float DesiredDist = Delta.Size();
				if (DesiredDist > KINDA_SMALL_NUMBER)
				{
					const float ActualDist = FVector::VectorPlaneProject(UpdatedComponent->GetComponentLocation() - OldLocation, GetComponentAxisZ()).Size();
					remainingTime += timeTick * (1.0f - FMath::Min(1.0f, ActualDist / DesiredDist));
				}

				StartNewPhysics(remainingTime, Iterations);
				return;
			}
			else if (IsSwimming())
			{
				//Just entered water.
				StartSwimmingOVERRIDEN(OldLocation, OldVelocity, timeTick, remainingTime, Iterations);
				return;
			}
		}

		// Update floor; StepUp might have already done it for us.
		if (StepDownResult.bComputedFloor)
		{
			CurrentFloor = StepDownResult.FloorResult;
		}
		else
		{
			FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, bZeroDelta, NULL);
		}

		// Check for ledges here.
		const bool bCheckLedges = !CanWalkOffLedges();
		if (bCheckLedges && !CurrentFloor.IsWalkableFloor())
		{
			// Calculate possible alternate movement.
			const FVector NewDelta = bTriedLedgeMove ? FVector::ZeroVector : GetLedgeMove(OldLocation, Delta, GetComponentAxisZ() * -1.0f);
			if (!NewDelta.IsZero())
			{
				// First revert this move.
				RevertMove(OldLocation, OldBase, PreviousBaseLocation, OldFloor, false);

				// Avoid repeated ledge moves if the first one fails.
				bTriedLedgeMove = true;

				// Try new movement direction.
				Velocity = NewDelta / timeTick;
				remainingTime += timeTick;
				continue;
			}
			else
			{
				// See if it is OK to jump.
				// @todo collision: only thing that can be problem is that OldBase has world collision on.
				bool bMustJump = bZeroDelta || OldBase == NULL || (!OldBase->IsQueryCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase));
				if ((bMustJump || !bCheckedFall) && CheckFall(OldFloor, CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump))
				{
					return;
				}

				bCheckedFall = true;

				// Revert this move.
				RevertMove(OldLocation, OldBase, PreviousBaseLocation, OldFloor, true);
				remainingTime = 0.0f;
				break;
			}
		}
		else
		{
			// Validate the floor check.
			if (CurrentFloor.IsWalkableFloor())
			{
				if (ShouldCatchAir(OldFloor, CurrentFloor))
				{
					CharacterOwner->OnWalkingOffLedge(OldFloor.HitResult.ImpactNormal, OldFloor.HitResult.Normal, OldLocation, timeTick);
					if (IsMovingOnGround())
					{
						// If still walking, then fall. If not, assume the user set a different mode they want to keep.
						StartFalling(Iterations, remainingTime, timeTick, Delta, OldLocation);
					}

					return;
				}

				AdjustFloorHeight();
				SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);
			}
			else if (CurrentFloor.HitResult.bStartPenetrating && remainingTime <= 0.0f)
			{
				// The floor check failed because it started in penetration.
				// We do not want to try to move downward because the downward sweep failed, rather we'd like to try to pop out of the floor.
				FHitResult Hit(CurrentFloor.HitResult);
				Hit.TraceEnd = Hit.TraceStart + GetComponentAxisZ() * MAX_FLOOR_DIST;
				const FVector RequestedAdjustment = GetPenetrationAdjustment(Hit);
				ResolvePenetration(RequestedAdjustment, Hit, UpdatedComponent->GetComponentQuat());
			}

			// Check if just entered water.
			if (IsSwimming())
			{
				StartSwimmingOVERRIDEN(OldLocation, Velocity, timeTick, remainingTime, Iterations);
				return;
			}

			// See if we need to start falling.
			if (!CurrentFloor.IsWalkableFloor() && !CurrentFloor.HitResult.bStartPenetrating)
			{
				const bool bMustJump = bJustTeleported || bZeroDelta || OldBase == NULL || (!OldBase->IsQueryCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase));
				if ((bMustJump || !bCheckedFall) && CheckFall(OldFloor, CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump))
				{
					return;
				}

				bCheckedFall = true;
			}
		}

		// Allow overlap events and such to change physics state and velocity.
		if (IsMovingOnGround())
		{
			// Make velocity reflect actual move.
			if (!bJustTeleported && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && timeTick >= MIN_TICK_TIME)
			{
				// TODO-RootMotionSource: Allow this to happen during partial override Velocity, but only set allowed axes?
				Velocity = (UpdatedComponent->GetComponentLocation() - OldLocation) / timeTick;
			}
		}

		// If we didn't move at all this iteration then abort (since future iterations will also be stuck).
		if (UpdatedComponent->GetComponentLocation() == OldLocation)
		{
			remainingTime = 0.0f;
			break;
		}
	}

	if (IsMovingOnGround())
	{
		MaintainHorizontalGroundVelocity();
	}
}

void UDashCharacterMovementComponent::AdjustFloorHeight()
{
	SCOPE_CYCLE_COUNTER(STAT_CharAdjustFloorHeight);

	// If we have a floor check that hasn't hit anything, don't adjust height.
	if (!CurrentFloor.bBlockingHit)
	{
		return;
	}

	const float OldFloorDist = CurrentFloor.FloorDist;
	if (CurrentFloor.bLineTrace && OldFloorDist < MIN_FLOOR_DIST)
	{
		// This would cause us to scale unwalkable walls.
		return;
	}

	// Move up or down to maintain floor height.
	if (OldFloorDist < MIN_FLOOR_DIST || OldFloorDist > MAX_FLOOR_DIST)
	{
		FHitResult AdjustHit(1.0f);
		const float AvgFloorDist = (MIN_FLOOR_DIST + MAX_FLOOR_DIST) * 0.5f;
		const float MoveDist = AvgFloorDist - OldFloorDist;
		const FVector CapsuleUp = GetComponentAxisZ();
		const FVector InitialLocation = UpdatedComponent->GetComponentLocation();

		SafeMoveUpdatedComponent(CapsuleUp * MoveDist, UpdatedComponent->GetComponentQuat(), true, AdjustHit);
		UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("Adjust floor height %.3f (Hit = %d)"), MoveDist, AdjustHit.bBlockingHit);

		if (!AdjustHit.IsValidBlockingHit())
		{
			CurrentFloor.FloorDist += MoveDist;
		}
		else if (MoveDist > 0.0f)
		{
			CurrentFloor.FloorDist += (InitialLocation - UpdatedComponent->GetComponentLocation()) | CapsuleUp;
		}
		else
		{
			checkSlow(MoveDist < 0.0f);

			CurrentFloor.FloorDist = (AdjustHit.Location - UpdatedComponent->GetComponentLocation()) | CapsuleUp;
			if (IsWalkable(AdjustHit))
			{
				CurrentFloor.SetFromSweep(AdjustHit, CurrentFloor.FloorDist, true);
			}
		}

		// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
		// Also avoid it if we moved out of penetration.
		bJustTeleported |= !bMaintainHorizontalGroundVelocity || OldFloorDist < 0.0f;
	}
}

void UDashCharacterMovementComponent::SetPostLandedPhysics(const FHitResult& Hit)
{
	if (CharacterOwner)
	{
		if (CanEverSwim() && IsInWater())
		{
			SetMovementMode(MOVE_Swimming);
		}
		else
		{
			const FVector PreImpactAccel = Acceleration + (IsFalling() ? GetGravity() : FVector::ZeroVector);
			const FVector PreImpactVelocity = Velocity;

			if (DefaultLandMovementMode == MOVE_Walking || DefaultLandMovementMode == MOVE_NavWalking || DefaultLandMovementMode == MOVE_Falling)
			{
				//				SetMovementMode(GroundMovementMode);
				SetMovementMode(GetGroundMovementMode()); //OVERRIDEN
			}
			else
			{
				SetDefaultMovementMode();
			}

			ApplyImpactPhysicsForces(Hit, PreImpactAccel, PreImpactVelocity);
		}
	}
}

void UDashCharacterMovementComponent::OnTeleported()
{
	if (!HasValidData())
	{
		return;
	}

	bool bWasFalling = (MovementMode == MOVE_Falling);
	bJustTeleported = true;

	// Find floor at current location.
	UpdateFloorFromAdjustment();

	// Validate it. We don't want to pop down to walking mode from very high off the ground, but we'd like to keep walking if possible.
	UPrimitiveComponent* OldBase = CharacterOwner->GetMovementBase();
	UPrimitiveComponent* NewBase = NULL;

	if (OldBase && CurrentFloor.IsWalkableFloor() && CurrentFloor.FloorDist <= MAX_FLOOR_DIST && (Velocity | GetComponentAxisZ()) <= 0.0f)
	{
		// Close enough to land or just keep walking.
		NewBase = CurrentFloor.HitResult.Component.Get();
	}
	else
	{
		CurrentFloor.Clear();
	}

	// If we were walking but no longer have a valid base or floor, start falling.
	const FVector SavedVelocity = Velocity;
	SetDefaultMovementMode();
	if (MovementMode == MOVE_Walking && (!CurrentFloor.IsWalkableFloor() || (OldBase && !NewBase)))
	{
		// If we are walking but no longer have a valid base or floor, start falling.
		Velocity = SavedVelocity;
		SetMovementMode(MOVE_Falling);
	}

	if (bWasFalling && IsMovingOnGround())
	{
		ProcessLanded(CurrentFloor.HitResult, 0.0f, 0);
	}

	MaybeSaveBaseLocation();
}

void UDashCharacterMovementComponent::PhysicsRotation(float DeltaTime)
{
	if ((!bOrientRotationToMovement && !bUseControllerDesiredRotation) || !HasValidData() || (!CharacterOwner->Controller && !bRunPhysicsWithNoController))
	{
		return;
	}

	FRotator CurrentRotation = UpdatedComponent->GetComponentRotation(); // Normalized.
	CurrentRotation.DiagnosticCheckNaN(TEXT("CharacterMovementComponent::PhysicsRotation(): CurrentRotation"));

	FRotator DeltaRot = GetDeltaRotation(DeltaTime);
	DeltaRot.DiagnosticCheckNaN(TEXT("CharacterMovementComponent::PhysicsRotation(): GetDeltaRotation"));

	FRotator DesiredRotation = CurrentRotation;
	if (bOrientRotationToMovement)
	{
		DesiredRotation = ComputeOrientToMovementRotation(CurrentRotation, DeltaTime, DeltaRot);
	}
	else if (CharacterOwner->Controller && bUseControllerDesiredRotation)
	{
		DesiredRotation = CharacterOwner->Controller->GetDesiredRotation();
	}
	else
	{
		return;
	}

	// Always remain vertical when walking or falling.
	if (IsMovingOnGround() || IsFalling())
	{
		DesiredRotation = ConstrainComponentRotation(DesiredRotation);
	}
	else
	{
		DesiredRotation.Normalize();
	}

	// Accumulate a desired new rotation.
	const float AngleTolerance = 1e-3f;

	if (!CurrentRotation.Equals(DesiredRotation, AngleTolerance))
	{
		if (DeltaRot.Roll == DeltaRot.Yaw && DeltaRot.Yaw == DeltaRot.Pitch)
		{
			// Calculate the spherical interpolation between the two rotators.
			const FQuat CurrentQuat(CurrentRotation);
			const FQuat DesiredQuat(DesiredRotation);

			// Get shortest angle between quaternions.
			const float Angle = FMath::Acos(FMath::Abs(CurrentQuat | DesiredQuat)) * 2.0f;

			// Calculate percent of interpolation.
			const float Alpha = FMath::Min(FMath::DegreesToRadians(DeltaRot.Yaw) / Angle, 1.0f);

			DesiredRotation = (Alpha == 1.0f) ? DesiredRotation : FQuat::Slerp(CurrentQuat, DesiredQuat, Alpha).Rotator();
		}
		else
		{
			// Pitch.
			if (!FMath::IsNearlyEqual(CurrentRotation.Pitch, DesiredRotation.Pitch, AngleTolerance))
			{
				DesiredRotation.Pitch = FMath::FixedTurn(CurrentRotation.Pitch, DesiredRotation.Pitch, DeltaRot.Pitch);
			}

			// Yaw.
			if (!FMath::IsNearlyEqual(CurrentRotation.Yaw, DesiredRotation.Yaw, AngleTolerance))
			{
				DesiredRotation.Yaw = FMath::FixedTurn(CurrentRotation.Yaw, DesiredRotation.Yaw, DeltaRot.Yaw);
			}

			// Roll.
			if (!FMath::IsNearlyEqual(CurrentRotation.Roll, DesiredRotation.Roll, AngleTolerance))
			{
				DesiredRotation.Roll = FMath::FixedTurn(CurrentRotation.Roll, DesiredRotation.Roll, DeltaRot.Roll);
			}
		}

		// Set the new rotation.
		DesiredRotation.DiagnosticCheckNaN(TEXT("CharacterMovementComponent::PhysicsRotation(): DesiredRotation"));
		MoveUpdatedComponent(FVector::ZeroVector, DesiredRotation, true);
	}
}

void UDashCharacterMovementComponent::PhysicsVolumeChanged(class APhysicsVolume* NewVolume)
{
	if (!HasValidData())
	{
		return;
	}

	if (NewVolume && NewVolume->bWaterVolume)
	{
		// Just entered water.
		if (!CanEverSwim())
		{
			// AI needs to stop any current moves.
			/*if (PathFollowingComp.IsValid())
			{
				PathFollowingComp->AbortMove(*this, FPathFollowingResultFlags::MovementStop);
			}*/

			// AI needs to stop any current moves
			IPathFollowingAgentInterface* PFAgent = GetPathFollowingAgent();
			if (PFAgent)
			{
				//PathFollowingComp->AbortMove(*this, FPathFollowingResultFlags::MovementStop);
				PFAgent->OnUnableToMove(*this);
			}
		}
		else if (!IsSwimming())
		{
			SetMovementMode(MOVE_Swimming);
		}
	}
	else if (IsSwimming())
	{
		SetMovementMode(MOVE_Falling);

		// Just left the water, check if should jump out.
		const FVector GravityDir = GetGravityDirection(true);
		FVector JumpDir = FVector::ZeroVector;
		FVector WallNormal = FVector::ZeroVector;

		if ((Acceleration | GravityDir) < 0.0f && ShouldJumpOutOfWaterEx(JumpDir, GravityDir) && (JumpDir | Acceleration) > 0.0f && CheckWaterJumpEx(JumpDir, GravityDir, WallNormal))
		{
			JumpOutOfWater(WallNormal);
			Velocity = FVector::VectorPlaneProject(Velocity, GravityDir) - GravityDir * OutofWaterZ; // Set here so physics uses this for remainder of tick.
		}
	}
}

bool UDashCharacterMovementComponent::ShouldJumpOutOfWater(FVector& JumpDir)
{
	return ShouldJumpOutOfWaterEx(JumpDir, GetGravityDirection(true));
}

bool UDashCharacterMovementComponent::ShouldJumpOutOfWaterEx(FVector& JumpDir, const FVector& GravDir)
{
	// If pawn is going up and looking up, then make it jump.
	AController* OwnerController = CharacterOwner->GetController();
	if (OwnerController && (Velocity | GravDir) < 0.0f)
	{
		const FVector ControllerDir = OwnerController->GetControlRotation().Vector();
		if ((ControllerDir | GravDir) < FMath::Cos(FMath::DegreesToRadians(JumpOutOfWaterPitch + 90.0f)))
		{
			JumpDir = ControllerDir;
			return true;
		}
	}

	return false;
}

bool UDashCharacterMovementComponent::CheckWaterJump(FVector CheckPoint, FVector& WallNormal)
{
	return CheckWaterJumpEx(CheckPoint, GetGravityDirection(true), WallNormal);
}

bool UDashCharacterMovementComponent::CheckWaterJumpEx(FVector CheckPoint, const FVector& GravDir, FVector& WallNormal)
{
	if (!HasValidData())
	{
		return false;
	}

	// Check if there is a wall directly in front of the swimming pawn.
	float PawnCapsuleRadius, PawnCapsuleHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnCapsuleRadius, PawnCapsuleHalfHeight);
	CheckPoint = UpdatedComponent->GetComponentLocation() + FVector::VectorPlaneProject(CheckPoint, GravDir).GetSafeNormal() * (PawnCapsuleRadius * 1.2f);

	FCollisionQueryParams CapsuleParams(DashCharacterMovementComponentStatics::CheckWaterJumpName, false, CharacterOwner);
	const FCollisionShape CapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_None);
	const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();
	FCollisionResponseParams ResponseParam;
	InitCollisionParams(CapsuleParams, ResponseParam);

	FHitResult HitInfo(1.0f);
	bool bHit = GetWorld()->SweepSingleByChannel(HitInfo, UpdatedComponent->GetComponentLocation(), CheckPoint, UpdatedComponent->GetComponentQuat(), CollisionChannel, CapsuleShape, CapsuleParams, ResponseParam);

	if (bHit && !Cast<APawn>(HitInfo.GetActor()))
	{
		// Hit a wall, check if it's low enough.
		WallNormal = HitInfo.ImpactNormal * -1.0f;
		const FVector Start = UpdatedComponent->GetComponentLocation() + GravDir * -MaxOutOfWaterStepHeight;
		CheckPoint = Start + WallNormal * (PawnCapsuleRadius * 3.2f);

		FCollisionQueryParams LineParams(DashCharacterMovementComponentStatics::CheckWaterJumpName, true, CharacterOwner);
		FCollisionResponseParams LineResponseParam;
		InitCollisionParams(LineParams, LineResponseParam);

		HitInfo.Reset(1.0f, false);
		bHit = GetWorld()->LineTraceSingleByChannel(HitInfo, Start, CheckPoint, CollisionChannel, LineParams, LineResponseParam);

		// If no high obstruction, or it's a valid floor, then pawn can jump out of water.
		return !bHit || IsWalkable(HitInfo);
	}

	return false;
}

void UDashCharacterMovementComponent::MoveSmooth(const FVector& InVelocity, const float DeltaSeconds, FStepDownResult* OutStepDownResult)
{
	if (!HasValidData())
	{
		return;
	}

	// Custom movement mode.
	// Custom movement may need an update even if there is zero velocity.
	if (MovementMode == MOVE_Custom)
	{
		FScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);
		PhysCustom(DeltaSeconds, 0);
		return;
	}

	FVector Delta = InVelocity * DeltaSeconds;
	if (Delta.IsZero())
	{
		return;
	}

	FScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);

	if (IsMovingOnGround())
	{
		MoveAlongFloor(InVelocity, DeltaSeconds, OutStepDownResult);
	}
	else
	{
		FHitResult Hit(1.0f);
		SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);

		if (Hit.IsValidBlockingHit())
		{
			bool bSteppedUp = false;

			if (IsFlying())
			{
				if (CanStepUp(Hit))
				{
					OutStepDownResult = NULL; // No need for a floor when not walking.
					const FVector CapsuleDown = GetComponentAxisZ() * -1.0f;

					if (FMath::Abs(Hit.ImpactNormal | CapsuleDown) < 0.2f)
					{
						const float UpDown = CapsuleDown | Delta.GetSafeNormal();
						if (UpDown < 0.5f && UpDown > -0.2f)
						{
							bSteppedUp = StepUp(CapsuleDown, Delta * (1.0f - Hit.Time), Hit, OutStepDownResult);
						}
					}
				}
			}

			// If StepUp failed, try sliding.
			if (!bSteppedUp)
			{
				SlideAlongSurface(Delta, 1.0f - Hit.Time, Hit.Normal, Hit, false);
			}
		}
	}
}

bool UDashCharacterMovementComponent::IsWalkable(const FHitResult& Hit) const
{
	if (!Hit.IsValidBlockingHit())
	{
		// No hit, or starting in penetration.
		return false;
	}

	float TestWalkableZ = GetWalkableFloorZ();

	// See if this component overrides the walkable floor z.
	const UPrimitiveComponent* HitComponent = Hit.Component.Get();
	if (HitComponent)
	{
		const FWalkableSlopeOverride& SlopeOverride = HitComponent->GetWalkableSlopeOverride();
		TestWalkableZ = SlopeOverride.ModifyWalkableFloorZ(TestWalkableZ);
	}

	// Can't walk on this surface if it is too steep.
	if ((Hit.ImpactNormal | GetComponentAxisZ()) < TestWalkableZ)
	{
		return false;
	}

	return true;
}

bool UDashCharacterMovementComponent::IsWithinEdgeTolerance(const FVector& CapsuleLocation, const FVector& TestImpactPoint, const float CapsuleRadius) const
{
	return IsWithinEdgeToleranceEx(CapsuleLocation, GetComponentAxisZ() * -1.0f, CapsuleRadius, TestImpactPoint);
}

bool UDashCharacterMovementComponent::IsWithinEdgeToleranceEx(const FVector& CapsuleLocation, const FVector& CapsuleDown, const float CapsuleRadius, const FVector& TestImpactPoint) const
{
	const float DistFromCenterSq = (CapsuleLocation + CapsuleDown * ((TestImpactPoint - CapsuleLocation) | CapsuleDown) - TestImpactPoint).SizeSquared();
	const float ReducedRadiusSq = FMath::Square(FMath::Max(KINDA_SMALL_NUMBER, CapsuleRadius - SWEEP_EDGE_REJECT_DISTANCE));

	return DistFromCenterSq < ReducedRadiusSq;
}

void UDashCharacterMovementComponent::ComputeFloorDist(const FVector& CapsuleLocation, float LineDistance, float SweepDistance, FFindFloorResult& OutFloorResult, float SweepRadius, const FHitResult* DownwardSweepResult) const
{
	OutFloorResult.Clear();

	// No collision, no floor...
	if (!UpdatedComponent->IsQueryCollisionEnabled())
	{
		return;
	}

	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	const FVector CapsuleDown = GetComponentAxisZ() * -1.0f;

	bool bSkipSweep = false;
	if (DownwardSweepResult != NULL && DownwardSweepResult->IsValidBlockingHit())
	{
		const float Dot = CapsuleDown | ((DownwardSweepResult->TraceEnd - DownwardSweepResult->TraceStart).GetSafeNormal());

		// Only if the supplied sweep was vertical and downward.
		if (Dot >= THRESH_NORMALS_ARE_PARALLEL)
		{
			// Reject hits that are barely on the cusp of the radius of the capsule.
			if (IsWithinEdgeToleranceEx(DownwardSweepResult->Location, CapsuleDown, PawnRadius, DownwardSweepResult->ImpactPoint))
			{
				// Don't try a redundant sweep, regardless of whether this sweep is usable.
				bSkipSweep = true;

				const bool bIsWalkable = IsWalkable(*DownwardSweepResult);
				const float FloorDist = (CapsuleLocation - DownwardSweepResult->Location).Size();
				OutFloorResult.SetFromSweep(*DownwardSweepResult, FloorDist, bIsWalkable);

				if (bIsWalkable)
				{
					// Use the supplied downward sweep as the floor hit result.
					return;
				}
			}
		}
	}

	// We require the sweep distance to be >= the line distance, otherwise the HitResult can't be interpreted as the sweep result.
	if (SweepDistance < LineDistance)
	{
		check(SweepDistance >= LineDistance);
		return;
	}

	bool bBlockingHit = false;
	FCollisionQueryParams QueryParams(NAME_None, false, CharacterOwner);
	FCollisionResponseParams ResponseParam;
	InitCollisionParams(QueryParams, ResponseParam);
	const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();

	// Sweep test.
	if (!bSkipSweep && SweepDistance > 0.0f && SweepRadius > 0.0f)
	{
		// Use a shorter height to avoid sweeps giving weird results if we start on a surface.
		// This also allows us to adjust out of penetrations.
		const float ShrinkScale = 0.9f;
		const float ShrinkScaleOverlap = 0.1f;
		float ShrinkHeight = (PawnHalfHeight - PawnRadius) * (1.0f - ShrinkScale);
		float TraceDist = SweepDistance + ShrinkHeight;
		QueryParams.TraceTag = DashCharacterMovementComponentStatics::ComputeFloorDistName;
		FCollisionShape CapsuleShape = FCollisionShape::MakeCapsule(SweepRadius, PawnHalfHeight - ShrinkHeight);

		FHitResult Hit(1.0f);
		bBlockingHit = FloorSweepTest(Hit, CapsuleLocation, CapsuleLocation + CapsuleDown * TraceDist, CollisionChannel, CapsuleShape, QueryParams, ResponseParam);

		if (bBlockingHit)
		{
			// Reject hits adjacent to us, we only care about hits on the bottom portion of our capsule.
			// Check 2D distance to impact point, reject if within a tolerance from radius.
			if (Hit.bStartPenetrating || !IsWithinEdgeToleranceEx(CapsuleLocation, CapsuleDown, CapsuleShape.Capsule.Radius, Hit.ImpactPoint))
			{
				// Use a capsule with a slightly smaller radius and shorter height to avoid the adjacent object.
				ShrinkHeight = (PawnHalfHeight - PawnRadius) * (1.0f - ShrinkScaleOverlap);
				TraceDist = SweepDistance + ShrinkHeight;
				CapsuleShape.Capsule.Radius = FMath::Max(0.0f, CapsuleShape.Capsule.Radius - SWEEP_EDGE_REJECT_DISTANCE - KINDA_SMALL_NUMBER);
				CapsuleShape.Capsule.HalfHeight = FMath::Max(PawnHalfHeight - ShrinkHeight, CapsuleShape.Capsule.Radius);
				Hit.Reset(1.0f, false);

				bBlockingHit = FloorSweepTest(Hit, CapsuleLocation, CapsuleLocation + CapsuleDown * TraceDist, CollisionChannel, CapsuleShape, QueryParams, ResponseParam);
			}

			// Reduce hit distance by ShrinkHeight because we shrank the capsule for the trace.
			// We allow negative distances here, because this allows us to pull out of penetrations.
			const float MaxPenetrationAdjust = FMath::Max(MAX_FLOOR_DIST, PawnRadius);
			const float SweepResult = FMath::Max(-MaxPenetrationAdjust, Hit.Time * TraceDist - ShrinkHeight);

			OutFloorResult.SetFromSweep(Hit, SweepResult, false);
			if (Hit.IsValidBlockingHit() && IsWalkable(Hit))
			{
				if (SweepResult <= SweepDistance)
				{
					// Hit within test distance.
					OutFloorResult.bWalkableFloor = true;
					return;
				}
			}
		}
	}

	// Since we require a longer sweep than line trace, we don't want to run the line trace if the sweep missed everything.
	// We do however want to try a line trace if the sweep was stuck in penetration.
	if (!OutFloorResult.bBlockingHit && !OutFloorResult.HitResult.bStartPenetrating)
	{
		OutFloorResult.FloorDist = SweepDistance;
		return;
	}

	// Line trace.
	if (LineDistance > 0.0f)
	{
		const float ShrinkHeight = PawnHalfHeight;
		const FVector LineTraceStart = CapsuleLocation;
		const float TraceDist = LineDistance + ShrinkHeight;
		QueryParams.TraceTag = DashCharacterMovementComponentStatics::FloorLineTraceName;

		FHitResult Hit(1.0f);
		bBlockingHit = GetWorld()->LineTraceSingleByChannel(Hit, LineTraceStart, LineTraceStart + CapsuleDown * TraceDist,
			CollisionChannel, QueryParams, ResponseParam);

		if (bBlockingHit)
		{
			if (Hit.Time > 0.0f)
			{
				// Reduce hit distance by ShrinkHeight because we started the trace higher than the base.
				// We allow negative distances here, because this allows us to pull out of penetrations.
				const float MaxPenetrationAdjust = FMath::Max(MAX_FLOOR_DIST, PawnRadius);
				const float LineResult = FMath::Max(-MaxPenetrationAdjust, Hit.Time * TraceDist - ShrinkHeight);

				OutFloorResult.bBlockingHit = true;
				if (LineResult <= LineDistance && IsWalkable(Hit))
				{
					OutFloorResult.SetFromLineTrace(Hit, OutFloorResult.FloorDist, LineResult, true);
					return;
				}
			}
		}
	}

	// No hits were acceptable.
	OutFloorResult.bWalkableFloor = false;
	OutFloorResult.FloorDist = SweepDistance;
}

bool UDashCharacterMovementComponent::FloorSweepTest(FHitResult& OutHit, const FVector& Start, const FVector& End, ECollisionChannel TraceChannel,
	const FCollisionShape& CollisionShape, const FCollisionQueryParams& Params, const FCollisionResponseParams& ResponseParam) const
{
	bool bBlockingHit = false;

	if (!bUseFlatBaseForFloorChecks)
	{
		bBlockingHit = GetWorld()->SweepSingleByChannel(OutHit, Start, End, UpdatedComponent->GetComponentQuat(), TraceChannel, CollisionShape, Params, ResponseParam);
	}
	else
	{
		// Test with a box that is enclosed by the capsule.
		const float CapsuleRadius = CollisionShape.GetCapsuleRadius();
		const float CapsuleHeight = CollisionShape.GetCapsuleHalfHeight();
		const FCollisionShape BoxShape = FCollisionShape::MakeBox(FVector(CapsuleRadius * 0.707f, CapsuleRadius * 0.707f, CapsuleHeight));

		// Use a box rotation that ignores the capsule forward orientation.
		const FVector BoxUp = GetComponentAxisZ();
		const FQuat BoxRotation = FRotationMatrix::MakeFromZ(BoxUp).ToQuat();

		// First test with the box rotated so the corners are along the major axes (ie rotated 45 degrees).
		bBlockingHit = GetWorld()->SweepSingleByChannel(OutHit, Start, End, FQuat(BoxUp, PI * 0.25f) * BoxRotation, TraceChannel, BoxShape, Params, ResponseParam);

		if (!bBlockingHit)
		{
			// Test again with the same box, not rotated.
			OutHit.Reset(1.0f, false);
			bBlockingHit = GetWorld()->SweepSingleByChannel(OutHit, Start, End, BoxRotation, TraceChannel, BoxShape, Params, ResponseParam);
		}
	}

	return bBlockingHit;
}

bool UDashCharacterMovementComponent::IsValidLandingSpot(const FVector& CapsuleLocation, const FHitResult& Hit) const
{
	if (!Hit.bBlockingHit)
	{
		return false;
	}

	const FVector CapsuleDown = GetComponentAxisZ() * -1.0f;

	// Skip some checks if penetrating. Penetration will be handled by the FindFloor call (using a smaller capsule).
	if (!Hit.bStartPenetrating)
	{
		// Reject unwalkable floor normals.
		if (!IsWalkable(Hit))
		{
			return false;
		}

		float PawnRadius, PawnHalfHeight;
		CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

		// Get the axis of the capsule bounded by the following two end points.
		const FVector BottomPoint = Hit.Location + CapsuleDown * FMath::Max(0.0f, PawnHalfHeight - PawnRadius);
		const FVector TopPoint = Hit.Location - CapsuleDown;
		const FVector Segment = TopPoint - BottomPoint;

		// Project the impact point on the segment.
		const float Alpha = ((Hit.ImpactPoint - BottomPoint) | Segment) / Segment.SizeSquared();

		// Reject hits that are above our lower hemisphere (can happen when sliding "down" a vertical surface).
		if (Alpha >= 0.0f)
		{
			return false;
		}

		// Reject hits that are barely on the cusp of the radius of the capsule.
		if (!IsWithinEdgeToleranceEx(Hit.Location, CapsuleDown, PawnRadius, Hit.ImpactPoint))
		{
			return false;
		}
	}
	else
	{
		// Penetrating.
		if ((Hit.Normal | CapsuleDown) > -KINDA_SMALL_NUMBER)
		{
			// Normal is nearly horizontal or downward, that's a penetration adjustment next to a vertical or overhanging wall. Don't pop to the floor.
			return false;
		}
	}

	FFindFloorResult FloorResult;
	FindFloor(CapsuleLocation, FloorResult, false, &Hit);

	// Reject invalid surfaces.
	if (!FloorResult.IsWalkableFloor())
	{
		return false;
	}

	return true;
}

bool UDashCharacterMovementComponent::ShouldCheckForValidLandingSpot(float DeltaTime, const FVector& Delta, const FHitResult& Hit) const
{
	const FVector CapsuleUp = GetComponentAxisZ();

	// See if we hit an edge of a surface on the lower portion of the capsule.
	// In this case the normal will not equal the impact normal, and a downward sweep may find a walkable surface on top of the edge.
	if ((Hit.Normal | CapsuleUp) > KINDA_SMALL_NUMBER && !Hit.Normal.Equals(Hit.ImpactNormal) &&
		IsWithinEdgeToleranceEx(UpdatedComponent->GetComponentLocation(), CapsuleUp * -1.0f, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius(), Hit.ImpactPoint))
	{
		return true;
	}

	return false;
}

bool UDashCharacterMovementComponent::ShouldComputePerchResult(const FHitResult& InHit, bool bCheckRadius) const
{
	if (!InHit.IsValidBlockingHit())
	{
		return false;
	}

	// Don't try to perch if the edge radius is very small.
	if (GetPerchRadiusThreshold() <= SWEEP_EDGE_REJECT_DISTANCE)
	{
		return false;
	}

	if (bCheckRadius)
	{
		const FVector CapsuleDown = GetComponentAxisZ() * -1.0f;
		const float DistFromCenterSq = (InHit.Location + CapsuleDown * ((InHit.ImpactPoint - InHit.Location) | CapsuleDown) - InHit.ImpactPoint).SizeSquared();
		const float StandOnEdgeRadiusSq = FMath::Square(GetValidPerchRadius());

		if (DistFromCenterSq <= StandOnEdgeRadiusSq)
		{
			// Already within perch radius.
			return false;
		}
	}

	return true;
}

bool UDashCharacterMovementComponent::ComputePerchResult(const float TestRadius, const FHitResult& InHit, const float InMaxFloorDist, FFindFloorResult& OutPerchFloorResult) const
{
	if (InMaxFloorDist <= 0.0f)
	{
		return false;
	}

	// Sweep further than actual requested distance, because a reduced capsule radius means we could miss some hits that the normal radius would contact.
	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	const FVector CapsuleDown = GetComponentAxisZ() * -1.0f;
	const float InHitAboveBase = (InHit.Location + CapsuleDown * ((InHit.ImpactPoint - InHit.Location) | CapsuleDown) -
		(InHit.Location + CapsuleDown * PawnHalfHeight)).Size();
	const float PerchLineDist = FMath::Max(0.0f, InMaxFloorDist - InHitAboveBase);
	const float PerchSweepDist = FMath::Max(0.0f, InMaxFloorDist);

	const float ActualSweepDist = PerchSweepDist + PawnRadius;
	ComputeFloorDist(InHit.Location, PerchLineDist, ActualSweepDist, OutPerchFloorResult, TestRadius);

	if (!OutPerchFloorResult.IsWalkableFloor())
	{
		return false;
	}
	else if (InHitAboveBase + OutPerchFloorResult.FloorDist > InMaxFloorDist)
	{
		// Hit something past max distance.
		OutPerchFloorResult.bWalkableFloor = false;
		return false;
	}

	return true;
}

bool UDashCharacterMovementComponent::StepUp(const FVector& GravDir, const FVector& Delta, const FHitResult& InHit, struct UCharacterMovementComponent::FStepDownResult* OutStepDownResult)
{
	SCOPE_CYCLE_COUNTER(STAT_CharStepUp);

	if (!CanStepUp(InHit) || MaxStepHeight <= 0.0f)
	{
		return false;
	}

	const FVector OldLocation = UpdatedComponent->GetComponentLocation();
	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	const FVector CapsuleDown = GetComponentAxisZ() * -1.0f;

	// Get the axis of the capsule bounded by the following two end points.
	const FVector BottomPoint = OldLocation + CapsuleDown * PawnHalfHeight;
	const FVector TopPoint = OldLocation - CapsuleDown * FMath::Max(0.0f, PawnHalfHeight - PawnRadius);
	const FVector Segment = TopPoint - BottomPoint;

	// Project the impact point on the segment.
	const float Alpha = ((InHit.ImpactPoint - BottomPoint) | Segment) / Segment.SizeSquared();

	// Don't bother stepping up if top of capsule is hitting something or if the impact is below us.
	if (Alpha > 1.0f || Alpha <= 0.0f)
	{
		return false;
	}

	// Gravity should be a normalized direction.
	ensure(GravDir.IsNormalized());

	float StepTravelUpHeight = MaxStepHeight;
	float StepTravelDownHeight = StepTravelUpHeight;
	const float StepSideZ = (InHit.ImpactNormal | GravDir) * -1.0f;
	FVector PawnInitialFloorBase = OldLocation + CapsuleDown * PawnHalfHeight;
	FVector PawnFloorPoint = PawnInitialFloorBase;

	if (IsMovingOnGround() && CurrentFloor.IsWalkableFloor())
	{
		// Since we float a variable amount off the floor, we need to enforce max step height off the actual point of impact with the floor.
		const float FloorDist = FMath::Max(0.0f, CurrentFloor.FloorDist);
		PawnInitialFloorBase += CapsuleDown * FloorDist;
		StepTravelUpHeight = FMath::Max(StepTravelUpHeight - FloorDist, 0.0f);
		StepTravelDownHeight = (MaxStepHeight + MAX_FLOOR_DIST * 2.0f);

		const bool bHitVerticalFace = !IsWithinEdgeToleranceEx(InHit.Location, CapsuleDown, PawnRadius, InHit.ImpactPoint);
		if (!CurrentFloor.bLineTrace && !bHitVerticalFace)
		{
			PawnFloorPoint = CurrentFloor.HitResult.ImpactPoint;
		}
		else
		{
			// Base floor point is the base of the capsule moved down by how far we are hovering over the surface we are hitting.
			PawnFloorPoint += CapsuleDown * CurrentFloor.FloorDist;
		}
	}

	// Scope our movement updates, and do not apply them until all intermediate moves are completed.
	FScopedMovementUpdate ScopedStepUpMovement(UpdatedComponent, EScopedUpdate::DeferredUpdates);

	// Step up, treat as vertical wall.
	FHitResult SweepUpHit(1.0f);
	const FQuat PawnRotation = UpdatedComponent->GetComponentQuat();
	MoveUpdatedComponent(GravDir * -StepTravelUpHeight, PawnRotation, true, &SweepUpHit);

	if (SweepUpHit.bStartPenetrating)
	{
		// Undo movement.
		ScopedStepUpMovement.RevertMove();
		return false;
	}

	// Step forward.
	FHitResult Hit(1.0f);
	MoveUpdatedComponent(Delta, PawnRotation, true, &Hit);

	// Check result of forward movement.
	if (Hit.bBlockingHit)
	{
		if (Hit.bStartPenetrating)
		{
			// Undo movement.
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// If we hit something above us and also something ahead of us, we should notify about the upward hit as well.
		// The forward hit will be handled later (in the bSteppedOver case below).
		// In the case of hitting something above but not forward, we are not blocked from moving so we don't need the notification.
		if (SweepUpHit.bBlockingHit && Hit.bBlockingHit)
		{
			HandleImpact(SweepUpHit);
		}

		// Pawn ran into a wall.
		HandleImpact(Hit);
		if (IsFalling())
		{
			return true;
		}

		// Adjust and try again.
		const float ForwardHitTime = Hit.Time;
		const float ForwardSlideAmount = SlideAlongSurface(Delta, 1.0f - Hit.Time, Hit.Normal, Hit, true);

		if (IsFalling())
		{
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// If both the forward hit and the deflection got us nowhere, there is no point in this step up.
		if (ForwardHitTime == 0.0f && ForwardSlideAmount == 0.0f)
		{
			ScopedStepUpMovement.RevertMove();
			return false;
		}
	}

	// Step down.
	MoveUpdatedComponent(GravDir * StepTravelDownHeight, UpdatedComponent->GetComponentQuat(), true, &Hit);

	// If step down was initially penetrating abort the step up.
	if (Hit.bStartPenetrating)
	{
		ScopedStepUpMovement.RevertMove();
		return false;
	}

	FStepDownResult StepDownResult;
	if (Hit.IsValidBlockingHit())
	{
		// See if this step sequence would have allowed us to travel higher than our max step height allows.
		const float DeltaZ = (PawnFloorPoint - Hit.ImpactPoint) | CapsuleDown;
		if (DeltaZ > MaxStepHeight)
		{
			//UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (too high Height %.3f) up from floor base"), DeltaZ);
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// Reject unwalkable surface normals here.
		if (!IsWalkable(Hit))
		{
			// Reject if normal opposes movement direction.
			const bool bNormalTowardsMe = (Delta | Hit.ImpactNormal) < 0.0f;
			if (bNormalTowardsMe)
			{
				//UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (unwalkable normal %s opposed to movement)"), *Hit.ImpactNormal.ToString());
				ScopedStepUpMovement.RevertMove();
				return false;
			}

			// Also reject if we would end up being higher than our starting location by stepping down.
			// It's fine to step down onto an unwalkable normal below us, we will just slide off. Rejecting those moves would prevent us from being able to walk off the edge.
			if (((OldLocation - Hit.Location) | CapsuleDown) > 0.0f)
			{
				//UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (unwalkable normal %s above old position)"), *Hit.ImpactNormal.ToString());
				ScopedStepUpMovement.RevertMove();
				return false;
			}
		}

		// Reject moves where the downward sweep hit something very close to the edge of the capsule. This maintains consistency with FindFloor as well.
		if (!IsWithinEdgeToleranceEx(Hit.Location, CapsuleDown, PawnRadius, Hit.ImpactPoint))
		{
			//UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (outside edge tolerance)"));
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// Don't step up onto invalid surfaces if traveling higher.
		if (DeltaZ > 0.0f && !CanStepUp(Hit))
		{
			//UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (up onto surface with !CanStepUp())"));
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// See if we can validate the floor as a result of this step down. In almost all cases this should succeed, and we can avoid computing the floor outside this method.
		if (OutStepDownResult != NULL)
		{
			FindFloor(UpdatedComponent->GetComponentLocation(), StepDownResult.FloorResult, false, &Hit);

			// Reject unwalkable normals if we end up higher than our initial height.
			// It's fine to walk down onto an unwalkable surface, don't reject those moves.
			if (((OldLocation - Hit.Location) | CapsuleDown) > 0.0f)
			{
				// We should reject the floor result if we are trying to step up an actual step where we are not able to perch (this is rare).
				// In those cases we should instead abort the step up and try to slide along the stair.
				if (!StepDownResult.FloorResult.bBlockingHit && StepSideZ < MAX_STEP_SIDE_Z)
				{
					ScopedStepUpMovement.RevertMove();
					return false;
				}
			}

			StepDownResult.bComputedFloor = true;
		}
	}

	// Copy step down result.
	if (OutStepDownResult != NULL)
	{
		*OutStepDownResult = StepDownResult;
	}

	// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
	bJustTeleported |= !bMaintainHorizontalGroundVelocity;

	return true;
}

void UDashCharacterMovementComponent::HandleImpact(const FHitResult& Impact, float TimeSlice, const FVector& MoveDelta)
{
	if (CharacterOwner)
	{
		CharacterOwner->MoveBlockedBy(Impact);
	}

	IPathFollowingAgentInterface* PFAgent = GetPathFollowingAgent();
	if (PFAgent)
	{
		// Also notify path following!
		PFAgent->OnMoveBlockedBy(Impact);
	}

	APawn* OtherPawn = Cast<APawn>(Impact.GetActor());
	if (OtherPawn)
	{
		NotifyBumpedPawn(OtherPawn);
	}

	if (bEnablePhysicsInteraction)
	{
		const FVector ForceAccel = Acceleration + (IsFalling() ? GetGravity() : FVector::ZeroVector);
		ApplyImpactPhysicsForces(Impact, ForceAccel, Velocity);
	}
}

void UDashCharacterMovementComponent::ApplyImpactPhysicsForces(const FHitResult& Impact, const FVector& ImpactAcceleration, const FVector& ImpactVelocity)
{
	if (bEnablePhysicsInteraction && Impact.bBlockingHit)
	{
		UPrimitiveComponent* ImpactComponent = Impact.GetComponent();
		if (ImpactComponent != NULL && ImpactComponent->IsAnySimulatingPhysics())
		{
			FVector ForcePoint = Impact.ImpactPoint;
			FBodyInstance* BI = ImpactComponent->GetBodyInstance(Impact.BoneName);
			float BodyMass = 1.0f;

			if (BI != NULL)
			{
				BodyMass = FMath::Max(BI->GetBodyMass(), 1.0f);

				if (bPushForceUsingZOffset)
				{
					FVector Center, Extents;
					BI->GetBodyBounds().GetCenterAndExtents(Center, Extents);

					if (!Extents.IsNearlyZero())
					{
						const FVector CapsuleUp = GetComponentAxisZ();

						// Project impact point onto the horizontal plane defined by center and gravity, then offset from there.
						ForcePoint = FVector::PointPlaneProject(ForcePoint, Center, CapsuleUp) +
							CapsuleUp * (FMath::Abs(Extents | CapsuleUp) * PushForcePointZOffsetFactor);
					}
				}
			}

			FVector Force = Impact.ImpactNormal * -1.0f;
			float PushForceModificator = 1.0f;
			const FVector ComponentVelocity = ImpactComponent->GetPhysicsLinearVelocity();
			const FVector VirtualVelocity = ImpactAcceleration.IsZero() ? ImpactVelocity : ImpactAcceleration.GetSafeNormal() * GetMaxSpeed();
			float Dot = 0.0f;

			if (bScalePushForceToVelocity && !ComponentVelocity.IsNearlyZero())
			{
				Dot = ComponentVelocity | VirtualVelocity;

				if (Dot > 0.0f && Dot < 1.0f)
				{
					PushForceModificator *= Dot;
				}
			}

			if (bPushForceScaledToMass)
			{
				PushForceModificator *= BodyMass;
			}

			Force *= PushForceModificator;

			if (ComponentVelocity.IsNearlyZero())
			{
				Force *= InitialPushForceFactor;
				ImpactComponent->AddImpulseAtLocation(Force, ForcePoint, Impact.BoneName);
			}
			else
			{
				Force *= PushForceFactor;
				ImpactComponent->AddForceAtLocation(Force, ForcePoint, Impact.BoneName);
			}
		}
	}
}

void UDashCharacterMovementComponent::DisplayDebug(UCanvas* Canvas, const FDebugDisplayInfo& DebugDisplay, float& YL, float& YPos)
{
	if (CharacterOwner == NULL)
	{
		return;
	}

	FDisplayDebugManager& DisplayDebugManager = Canvas->DisplayDebugManager;
	DisplayDebugManager.SetDrawColor(FColor::White);
	FString T = FString::Printf(TEXT("CHARACTER MOVEMENT Floor %s Crouched %i"), *CurrentFloor.HitResult.ImpactNormal.ToString(), IsCrouching());
	DisplayDebugManager.DrawString(T);

	T = FString::Printf(TEXT("Updated Component: %s"), *UpdatedComponent->GetName());
	DisplayDebugManager.DrawString(T);

	T = FString::Printf(TEXT("Acceleration: %s"), *Acceleration.ToCompactString());
	DisplayDebugManager.DrawString(T);

	T = FString::Printf(TEXT("bForceMaxAccel: %i"), bForceMaxAccel);
	DisplayDebugManager.DrawString(T);

	T = FString::Printf(TEXT("RootMotionSources: %d active"), CurrentRootMotion.RootMotionSources.Num());
	DisplayDebugManager.DrawString(T);

	APhysicsVolume * PhysicsVolume = GetPhysicsVolume();

	const UPrimitiveComponent* BaseComponent = CharacterOwner->GetMovementBase();
	const AActor* BaseActor = BaseComponent ? BaseComponent->GetOwner() : NULL;

	T = FString::Printf(TEXT("%s In physicsvolume %s on base %s component %s gravity %s"), *GetMovementName(), (PhysicsVolume ? *PhysicsVolume->GetName() : TEXT("None")),
		(BaseActor ? *BaseActor->GetName() : TEXT("None")), (BaseComponent ? *BaseComponent->GetName() : TEXT("None")), *GetGravity().ToString());
	DisplayDebugManager.DrawString(T);
}

//float UCharacterMovementComponent::VisualizeMovement() const
//{
//	float HeightOffset = 0.f;
//	const float OffsetPerElement = 10.0f;
//	if (CharacterOwner == nullptr)
//	{
//		return HeightOffset;
//	}
//
//#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
//	const FVector TopOfCapsule = GetActorLocation() + FVector(0.f, 0.f, CharacterOwner->GetSimpleCollisionHalfHeight());
//	
//	// Position
//	{
//		const FColor DebugColor = FColor::White;
//		const FVector DebugLocation = TopOfCapsule + FVector(0.f,0.f,HeightOffset);
//		FString DebugText = FString::Printf(TEXT("Position: %s"), *GetActorLocation().ToCompactString());
//		DrawDebugString(GetWorld(), DebugLocation, DebugText, nullptr, DebugColor, 0.f, true);
//	}
//
//	// Velocity
//	{
//		const FColor DebugColor = FColor::Green;
//		HeightOffset += OffsetPerElement;
//		const FVector DebugLocation = TopOfCapsule + FVector(0.f,0.f,HeightOffset);
//		DrawDebugDirectionalArrow(GetWorld(), DebugLocation - FVector(0.f, 0.f, 5.0f), DebugLocation - FVector(0.f, 0.f, 5.0f) + Velocity,
//			100.f, DebugColor, false, -1.f, (uint8)'\000', 10.f);
//
//		FString DebugText = FString::Printf(TEXT("Velocity: %s (Speed: %.2f) (Max: %.2f)"), *Velocity.ToCompactString(), Velocity.Size(), GetMaxSpeed());
//		DrawDebugString(GetWorld(), DebugLocation, DebugText, nullptr, DebugColor, 0.f, true);
//	}
//
//	// Acceleration
//	{
//		const FColor DebugColor = FColor::Yellow;
//		HeightOffset += OffsetPerElement;
//		const float MaxAccelerationLineLength = 200.f;
//		const float CurrentMaxAccel = GetMaxAcceleration();
//		const float CurrentAccelAsPercentOfMaxAccel = CurrentMaxAccel > 0.f ? Acceleration.Size() / CurrentMaxAccel : 1.f;
//		const FVector DebugLocation = TopOfCapsule + FVector(0.f,0.f,HeightOffset);
//		DrawDebugDirectionalArrow(GetWorld(), DebugLocation - FVector(0.f, 0.f, 5.0f), 
//			DebugLocation - FVector(0.f, 0.f, 5.0f) + Acceleration.GetSafeNormal(SMALL_NUMBER) * CurrentAccelAsPercentOfMaxAccel * MaxAccelerationLineLength,
//			25.f, DebugColor, false, -1.f, (uint8)'\000', 8.f);
//
//		FString DebugText = FString::Printf(TEXT("Acceleration: %s"), *Acceleration.ToCompactString());
//		DrawDebugString(GetWorld(), DebugLocation, DebugText, nullptr, DebugColor, 0.f, true);
//	}
//
//	// Movement Mode
//	{
//		const FColor DebugColor = FColor::Blue;
//		HeightOffset += OffsetPerElement;
//		FVector DebugLocation = TopOfCapsule + FVector(0.f,0.f,HeightOffset);
//		FString DebugText = FString::Printf(TEXT("MovementMode: %s"), *GetMovementName());
//		DrawDebugString(GetWorld(), DebugLocation, DebugText, nullptr, DebugColor, 0.f, true);
//
//		if (IsInWater())
//		{
//			HeightOffset += OffsetPerElement;
//			DebugLocation = TopOfCapsule + FVector(0.f, 0.f, HeightOffset);
//			DebugText = FString::Printf(TEXT("ImmersionDepth: %.2f"), ImmersionDepth());
//			DrawDebugString(GetWorld(), DebugLocation, DebugText, nullptr, DebugColor, 0.f, true);
//		}
//	}
//
//	// Jump
//	{
//		const FColor DebugColor = FColor::Blue;
//		HeightOffset += OffsetPerElement;
//		FVector DebugLocation = TopOfCapsule + FVector(0.f, 0.f, HeightOffset);
//		FString DebugText = FString::Printf(TEXT("bIsJumping: %d Count: %d HoldTime: %.2f"), CharacterOwner->bPressedJump, CharacterOwner->JumpCurrentCount, CharacterOwner->JumpKeyHoldTime);
//		DrawDebugString(GetWorld(), DebugLocation, DebugText, nullptr, DebugColor, 0.f, true);
//	}
//
//	// Root motion (additive)
//	if (CurrentRootMotion.HasAdditiveVelocity())
//	{
//		const FColor DebugColor = FColor::Cyan;
//		HeightOffset += OffsetPerElement;
//		const FVector DebugLocation = TopOfCapsule + FVector(0.f,0.f,HeightOffset);
//
//		FVector CurrentAdditiveVelocity(FVector::ZeroVector);
//		CurrentRootMotion.AccumulateAdditiveRootMotionVelocity(0.f, *CharacterOwner, *this, CurrentAdditiveVelocity);
//
//		DrawDebugDirectionalArrow(GetWorld(), DebugLocation, DebugLocation + CurrentAdditiveVelocity, 
//			100.f, DebugColor, false, -1.f, (uint8)'\000', 10.f);
//
//		FString DebugText = FString::Printf(TEXT("RootMotionAdditiveVelocity: %s (Speed: %.2f)"), 
//			*CurrentAdditiveVelocity.ToCompactString(), CurrentAdditiveVelocity.Size());
//		DrawDebugString(GetWorld(), DebugLocation + FVector(0.f,0.f,5.f), DebugText, nullptr, DebugColor, 0.f, true);
//	}
//
//	// Root motion (override)
//	if (CurrentRootMotion.HasOverrideVelocity())
//	{
//		const FColor DebugColor = FColor::Green;
//		HeightOffset += OffsetPerElement;
//		const FVector DebugLocation = TopOfCapsule + FVector(0.f,0.f,HeightOffset);
//		FString DebugText = FString::Printf(TEXT("Has Override RootMotion"));
//		DrawDebugString(GetWorld(), DebugLocation, DebugText, nullptr, DebugColor, 0.f, true);
//	}
//#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
//
//	return HeightOffset;
//}

FVector UDashCharacterMovementComponent::ConstrainInputAcceleration(const FVector& InputAcceleration) const
{
	FVector NewAccel = InputAcceleration;

	// Walking or falling pawns ignore up/down sliding.
	if (IsMovingOnGround() || IsFalling())
	{
		NewAccel = FVector::VectorPlaneProject(NewAccel, GetComponentAxisZ());
	}

	return NewAccel;
}

void UDashCharacterMovementComponent::ServerMoveHandleClientError(float ClientTimeStamp, float DeltaTime, const FVector& Accel, const FVector& RelativeClientLoc, UPrimitiveComponent* ClientMovementBase, FName ClientBaseBoneName, uint8 ClientMovementMode)
{
	if (RelativeClientLoc == FVector(1.0f, 2.0f, 3.0f)) // First part of double servermove.
	{
		return;
	}

	FNetworkPredictionData_Server_Character* ServerData = GetPredictionData_Server_Character();
	check(ServerData);

	// Don't prevent more recent updates from being sent if received this frame.
	// We're going to send out an update anyway, might as well be the most recent one.
	APlayerController* PC = Cast<APlayerController>(CharacterOwner->GetController());
	if (ServerData->LastUpdateTime != GetWorld()->TimeSeconds && GetDefault<AGameNetworkManager>()->WithinUpdateDelayBounds(PC, ServerData->LastUpdateTime))
	{
		return;
	}

	// Offset may be relative to base component.
	FVector ClientLoc = RelativeClientLoc;
	if (MovementBaseUtility::UseRelativeLocation(ClientMovementBase))
	{
		FVector BaseLocation;
		FQuat BaseRotation;
		MovementBaseUtility::GetMovementBaseTransform(ClientMovementBase, ClientBaseBoneName, BaseLocation, BaseRotation);
		ClientLoc += BaseLocation;
	}

	// Compute the client error from the server's position.
	// If client has accumulated a noticeable positional error, correct him.
	if (ServerData->bForceClientUpdate || ServerCheckClientError(ClientTimeStamp, DeltaTime, Accel, ClientLoc, RelativeClientLoc, ClientMovementBase, ClientBaseBoneName, ClientMovementMode))
	{
		UPrimitiveComponent* MovementBase = CharacterOwner->GetMovementBase();
		ServerData->PendingAdjustment.NewVel = Velocity;
		ServerData->PendingAdjustment.NewBase = MovementBase;
		ServerData->PendingAdjustment.NewBaseBoneName = CharacterOwner->GetBasedMovement().BoneName;
		ServerData->PendingAdjustment.NewLoc = UpdatedComponent->GetComponentLocation();
		ServerData->PendingAdjustment.NewRot = UpdatedComponent->GetComponentRotation();

		ServerData->PendingAdjustment.bBaseRelativePosition = MovementBaseUtility::UseRelativeLocation(MovementBase);
		if (ServerData->PendingAdjustment.bBaseRelativePosition)
		{
			// Relative location.
			ServerData->PendingAdjustment.NewLoc = CharacterOwner->GetBasedMovement().Location;

			// TODO: this could be a relative rotation, but all client corrections ignore rotation right now except the root motion one, which would need to be updated.
			//ServerData->PendingAdjustment.NewRot = CharacterOwner->GetBasedMovement().Rotation;
		}

#if !UE_BUILD_SHIPPING
		if (DashCharacterMovementCVars::NetShowCorrections != 0)
		{
			const FVector LocDiff = UpdatedComponent->GetComponentLocation() - ClientLoc;
			const FString BaseString = MovementBase ? MovementBase->GetPathName(MovementBase->GetOutermost()) : TEXT("None");
			UE_LOG(LogNetPlayerMovement, Warning, TEXT("*** Server: Error for %s at Time=%.3f is %3.3f LocDiff(%s) ClientLoc(%s) ServerLoc(%s) Base: %s Bone: %s Accel(%s) Velocity(%s)"),
				*GetNameSafe(CharacterOwner), ClientTimeStamp, LocDiff.Size(), *LocDiff.ToString(), *ClientLoc.ToString(), *UpdatedComponent->GetComponentLocation().ToString(), *BaseString, *ServerData->PendingAdjustment.NewBaseBoneName.ToString(), *Accel.ToString(), *Velocity.ToString());
			const float DebugLifetime = DashCharacterMovementCVars::NetCorrectionLifetime;
			DrawDebugCapsule(GetWorld(), UpdatedComponent->GetComponentLocation(), CharacterOwner->GetSimpleCollisionHalfHeight(), CharacterOwner->GetSimpleCollisionRadius(), UpdatedComponent->GetComponentQuat(), FColor(100, 255, 100), true, DebugLifetime);
			DrawDebugCapsule(GetWorld(), ClientLoc, CharacterOwner->GetSimpleCollisionHalfHeight(), CharacterOwner->GetSimpleCollisionRadius(), UpdatedComponent->GetComponentQuat(), FColor(255, 100, 100), true, DebugLifetime);
		}
#endif

		ServerData->LastUpdateTime = GetWorld()->TimeSeconds;
		ServerData->PendingAdjustment.DeltaTime = DeltaTime;
		ServerData->PendingAdjustment.TimeStamp = ClientTimeStamp;
		ServerData->PendingAdjustment.bAckGoodMove = false;
		ServerData->PendingAdjustment.MovementMode = PackNetworkMovementMode();

		PerfCountersIncrement(TEXT("NumServerMoveCorrections"));
	}
	else
	{
		if (GetDefault<AGameNetworkManager>()->ClientAuthorativePosition)
		{
			const FVector LocDiff = UpdatedComponent->GetComponentLocation() - ClientLoc;
			if (!LocDiff.IsZero() || ClientMovementMode != PackNetworkMovementMode() || GetMovementBase() != ClientMovementBase || (CharacterOwner && CharacterOwner->GetBasedMovement().BoneName != ClientBaseBoneName))
			{
				// Just set the position. On subsequent moves we will resolve initially overlapping conditions.
				UpdatedComponent->SetWorldLocation(ClientLoc, false);

				// Trust the client's movement mode.
				ApplyNetworkMovementMode(ClientMovementMode);

				// Update base and floor at new location.
				SetBase(ClientMovementBase, ClientBaseBoneName);
				UpdateFloorFromAdjustment();

				// Even if base has not changed, we need to recompute the relative offsets (since we've moved).
				SaveBaseLocation();

				LastUpdateLocation = UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector;
				LastUpdateRotation = UpdatedComponent ? UpdatedComponent->GetComponentQuat() : FQuat::Identity;
				LastUpdateVelocity = Velocity;
			}
		}

		// Acknowledge receipt of this successful ServerMove().
		ServerData->PendingAdjustment.TimeStamp = ClientTimeStamp;
		ServerData->PendingAdjustment.bAckGoodMove = true;
	}

	PerfCountersIncrement(TEXT("NumServerMoves"));

	ServerData->bForceClientUpdate = false;
}

void UDashCharacterMovementComponent::ClientAdjustPosition_Implementation(float TimeStamp, FVector NewLocation, FVector NewVelocity, UPrimitiveComponent* NewBase, FName NewBaseBoneName, bool bHasBase, bool bBaseRelativePosition, uint8 ServerMovementMode)
{
	if (!HasValidData() || !IsComponentTickEnabled())
	{
		return;
	}

	FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
	check(ClientData);

	// Make sure the base actor exists on this client.
	const bool bUnresolvedBase = bHasBase && NewBase == NULL;
	if (bUnresolvedBase)
	{
		if (bBaseRelativePosition)
		{
			UE_LOG(LogNetPlayerMovement, Warning, TEXT("ClientAdjustPosition_Implementation could not resolve the new relative movement base actor, ignoring server correction!"));
			return;
		}
		else
		{
			UE_LOG(LogNetPlayerMovement, Verbose, TEXT("ClientAdjustPosition_Implementation could not resolve the new absolute movement base actor, but WILL use the position!"));
		}
	}

	// Ack move if it has not expired.
	int32 MoveIndex = ClientData->GetSavedMoveIndex(TimeStamp);
	if (MoveIndex == INDEX_NONE)
	{
		if (ClientData->LastAckedMove.IsValid())
		{
			UE_LOG(LogNetPlayerMovement, Log, TEXT("ClientAdjustPosition_Implementation could not find Move for TimeStamp: %f, LastAckedTimeStamp: %f, CurrentTimeStamp: %f"), TimeStamp, ClientData->LastAckedMove->TimeStamp, ClientData->CurrentTimeStamp);
		}
		return;
	}
	ClientData->AckMove(MoveIndex, *this);

	// Received Location is relative to dynamic base.
	if (bBaseRelativePosition)
	{
		FVector BaseLocation;
		FQuat BaseRotation;
		MovementBaseUtility::GetMovementBaseTransform(NewBase, NewBaseBoneName, BaseLocation, BaseRotation); // TODO: error handling if returns false.
		NewLocation += BaseLocation;
	}

#if !UE_BUILD_SHIPPING
	if (DashCharacterMovementCVars::NetShowCorrections != 0)
	{
		const FVector LocDiff = UpdatedComponent->GetComponentLocation() - NewLocation;
		const FString NewBaseString = NewBase ? NewBase->GetPathName(NewBase->GetOutermost()) : TEXT("None");
		UE_LOG(LogNetPlayerMovement, Warning, TEXT("*** Client: Error for %s at Time=%.3f is %3.3f LocDiff(%s) ClientLoc(%s) ServerLoc(%s) NewBase: %s NewBone: %s ClientVel(%s) ServerVel(%s) SavedMoves %d"),
			*GetNameSafe(CharacterOwner), TimeStamp, LocDiff.Size(), *LocDiff.ToString(), *UpdatedComponent->GetComponentLocation().ToString(), *NewLocation.ToString(), *NewBaseString, *NewBaseBoneName.ToString(), *Velocity.ToString(), *NewVelocity.ToString(), ClientData->SavedMoves.Num());
		const float DebugLifetime = DashCharacterMovementCVars::NetCorrectionLifetime;
		DrawDebugCapsule(GetWorld(), UpdatedComponent->GetComponentLocation(), CharacterOwner->GetSimpleCollisionHalfHeight(), CharacterOwner->GetSimpleCollisionRadius(), UpdatedComponent->GetComponentQuat(), FColor(255, 100, 100), true, DebugLifetime);
		DrawDebugCapsule(GetWorld(), NewLocation, CharacterOwner->GetSimpleCollisionHalfHeight(), CharacterOwner->GetSimpleCollisionRadius(), UpdatedComponent->GetComponentQuat(), FColor(100, 255, 100), true, DebugLifetime);
	}
#endif //!UE_BUILD_SHIPPING

	// Trust the server's positioning.
	UpdatedComponent->SetWorldLocation(NewLocation, false);
	Velocity = NewVelocity;

	// Trust the server's movement mode.
	UPrimitiveComponent* PreviousBase = CharacterOwner->GetMovementBase();
	ApplyNetworkMovementMode(ServerMovementMode);

	// Set base component.
	UPrimitiveComponent* FinalBase = NewBase;
	FName FinalBaseBoneName = NewBaseBoneName;
	if (bUnresolvedBase)
	{
		check(NewBase == NULL);
		check(!bBaseRelativePosition);

		// We had an unresolved base from the server.
		// If walking, we'd like to continue walking if possible, to avoid falling for a frame, so try to find a base where we moved to.
		if (PreviousBase)
		{
			FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, false);
			if (CurrentFloor.IsWalkableFloor())
			{
				FinalBase = CurrentFloor.HitResult.Component.Get();
				FinalBaseBoneName = CurrentFloor.HitResult.BoneName;
			}
			else
			{
				FinalBase = nullptr;
				FinalBaseBoneName = NAME_None;
			}
		}
	}
	SetBase(FinalBase, FinalBaseBoneName);

	// Update floor at new location.
	UpdateFloorFromAdjustment();
	bJustTeleported = true;

	// Even if base has not changed, we need to recompute the relative offsets (since we've moved).
	SaveBaseLocation();

	LastUpdateLocation = UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector;
	LastUpdateRotation = UpdatedComponent ? UpdatedComponent->GetComponentQuat() : FQuat::Identity;
	LastUpdateVelocity = Velocity;

	UpdateComponentVelocity();
	ClientData->bUpdatePosition = true;
}

void UDashCharacterMovementComponent::CapsuleTouched(UPrimitiveComponent* OverlappedComp, AActor* Other, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (!bEnablePhysicsInteraction)
	{
		return;
	}

	if (OtherComp != NULL && OtherComp->IsAnySimulatingPhysics())
	{
		const FVector OtherLoc = OtherComp->GetComponentLocation();
		const FVector Loc = UpdatedComponent->GetComponentLocation();
		const FVector CapsuleUp = GetComponentAxisZ();

		FVector ImpulseDir = FVector::VectorPlaneProject(OtherLoc - Loc, CapsuleUp) + CapsuleUp * 0.25f;
		ImpulseDir = (ImpulseDir.GetSafeNormal() + FVector::VectorPlaneProject(Velocity, CapsuleUp).GetSafeNormal()) * 0.5f;
		ImpulseDir.Normalize();

		FName BoneName = NAME_None;
		if (OtherBodyIndex != INDEX_NONE)
		{
			BoneName = ((USkinnedMeshComponent*)OtherComp)->GetBoneName(OtherBodyIndex);
		}

		float TouchForceFactorModified = TouchForceFactor;

		if (bTouchForceScaledToMass)
		{
			FBodyInstance* BI = OtherComp->GetBodyInstance(BoneName);
			TouchForceFactorModified *= BI ? BI->GetBodyMass() : 1.0f;
		}

		float ImpulseStrength = FMath::Clamp(FVector::VectorPlaneProject(Velocity, CapsuleUp).Size() * TouchForceFactorModified,
			MinTouchForce > 0.0f ? MinTouchForce : -FLT_MAX, MaxTouchForce > 0.0f ? MaxTouchForce : FLT_MAX);

		FVector Impulse = ImpulseDir * ImpulseStrength;

		OtherComp->AddImpulse(Impulse, BoneName);
	}
}

void UDashCharacterMovementComponent::ApplyDownwardForce(float DeltaSeconds)
{
	if (StandingDownwardForceScale != 0.0f && CurrentFloor.HitResult.IsValidBlockingHit())
	{
		UPrimitiveComponent* BaseComp = CurrentFloor.HitResult.GetComponent();
		const FVector Gravity = GetGravity();

		if (BaseComp && BaseComp->IsAnySimulatingPhysics() && !Gravity.IsZero())
		{
			BaseComp->AddForceAtLocation(Gravity * Mass * StandingDownwardForceScale, CurrentFloor.HitResult.ImpactPoint, CurrentFloor.HitResult.BoneName);
		}
	}
}

void UDashCharacterMovementComponent::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	// Do not update velocity when using root motion or when SimulatedProxy - SimulatedProxy are repped their Velocity
	if (!HasValidData() || HasAnimRootMotion() || DeltaTime < MIN_TICK_TIME || (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy))
	{
		return;
	}

	Friction = FMath::Max(0.f, Friction);
	const float MaxAccel = GetMaxAcceleration();
	float MaxSpeed = GetMaxSpeed();

	// Check if path following requested movement
	bool bZeroRequestedAcceleration = true;
	FVector RequestedAcceleration = FVector::ZeroVector;
	float RequestedSpeed = 0.0f;
	if (ApplyRequestedMove(DeltaTime, MaxAccel, MaxSpeed, Friction, BrakingDeceleration, RequestedAcceleration, RequestedSpeed))
	{
		RequestedAcceleration = RequestedAcceleration.GetClampedToMaxSize(MaxAccel);
		bZeroRequestedAcceleration = false;
	}

	if (bForceMaxAccel)
	{
		// Force acceleration at full speed.
		// In consideration order for direction: Acceleration, then Velocity, then Pawn's rotation.
		if (Acceleration.SizeSquared() > SMALL_NUMBER)
		{
			Acceleration = Acceleration.GetSafeNormal() * MaxAccel;
		}
		else
		{
			Acceleration = MaxAccel * (Velocity.SizeSquared() < SMALL_NUMBER ? UpdatedComponent->GetForwardVector() : Velocity.GetSafeNormal());
		}

		AnalogInputModifier = 1.f;
	}

	// Path following above didn't care about the analog modifier, but we do for everything else below, so get the fully modified value.
	// Use max of requested speed and max speed if we modified the speed in ApplyRequestedMove above.
	MaxSpeed = FMath::Max3(RequestedSpeed, MaxSpeed * AnalogInputModifier, GetMinAnalogSpeed());

	// Apply braking or deceleration
	const bool bZeroAcceleration = Acceleration.IsZero();
	const bool bVelocityOverMax = false;

	// Only apply braking if there is no acceleration, or we are over our max speed and need to slow down to it.
	if ((bZeroAcceleration && bZeroRequestedAcceleration) || bVelocityOverMax)
	{
		const FVector OldVelocity = Velocity;

		const float ActualBrakingFriction = (bUseSeparateBrakingFriction ? BrakingFriction : Friction);
		ApplyVelocityBraking(DeltaTime, ActualBrakingFriction, BrakingDeceleration);

		// Don't allow braking to lower us below max speed if we started above it.
		if (bVelocityOverMax && Velocity.SizeSquared() < FMath::Square(MaxSpeed) && FVector::DotProduct(Acceleration, OldVelocity) > 0.0f)
		{
			Velocity = OldVelocity.GetSafeNormal() * MaxSpeed;
		}
	}
	else if (!bZeroAcceleration)
	{
		// Friction affects our ability to change direction. This is only done for input acceleration, not path following.
		const FVector AccelDir = Acceleration.GetSafeNormal();
		const float VelSize = Velocity.Size();
		Velocity = Velocity - (Velocity - AccelDir * VelSize) * FMath::Min(DeltaTime * Friction, 1.f);
	}

	// Apply fluid friction
	if (bFluid)
	{
		Velocity = Velocity * (1.f - FMath::Min(Friction * DeltaTime, 1.f));
	}

	// Apply acceleration
	const float NewMaxSpeed = (IsExceedingMaxSpeed(MaxSpeed)) ? Velocity.Size() : MaxSpeed;
	Velocity += Acceleration * DeltaTime;
	Velocity += RequestedAcceleration * DeltaTime;
	Velocity = Velocity.GetClampedToMaxSize(NewMaxSpeed);

	if (bUseRVOAvoidance)
	{
		CalcAvoidanceVelocity(DeltaTime);
	}
}


void UDashCharacterMovementComponent::ApplyRepulsionForce(float DeltaSeconds)
{
	if (UpdatedPrimitive && RepulsionForce > 0.0f)
	{
		const TArray<FOverlapInfo>& Overlaps = UpdatedPrimitive->GetOverlapInfos();
		if (Overlaps.Num() > 0)
		{
			FCollisionQueryParams QueryParams;
			QueryParams.bReturnFaceIndex = false;
			QueryParams.bReturnPhysicalMaterial = false;

			float CapsuleRadius = 0.0f;
			float CapsuleHalfHeight = 0.0f;
			CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(CapsuleRadius, CapsuleHalfHeight);
			const float RepulsionForceRadius = CapsuleRadius * 1.2f;
			const float StopBodyDistance = 2.5f;
			const FVector MyLocation = UpdatedPrimitive->GetComponentLocation();
			const FVector CapsuleDown = GetComponentAxisZ() * -1.0f;

			for (int32 i = 0; i < Overlaps.Num(); i++)
			{
				const FOverlapInfo& Overlap = Overlaps[i];

				UPrimitiveComponent* OverlapComp = Overlap.OverlapInfo.Component.Get();
				if (!OverlapComp || OverlapComp->Mobility < EComponentMobility::Movable)
				{
					continue;
				}

				// Use the body instead of the component for cases where we have multi-body overlaps enabled.
				FBodyInstance* OverlapBody = nullptr;
				const int32 OverlapBodyIndex = Overlap.GetBodyIndex();
				const USkeletalMeshComponent* SkelMeshForBody = (OverlapBodyIndex != INDEX_NONE) ? Cast<USkeletalMeshComponent>(OverlapComp) : nullptr;
				if (SkelMeshForBody != nullptr)
				{
					OverlapBody = SkelMeshForBody->Bodies.IsValidIndex(OverlapBodyIndex) ? SkelMeshForBody->Bodies[OverlapBodyIndex] : nullptr;
				}
				else
				{
					OverlapBody = OverlapComp->GetBodyInstance();
				}

				if (!OverlapBody)
				{
					UE_LOG(LogCharacterMovement, Warning, TEXT("%s could not find overlap body for body index %d"), *GetName(), OverlapBodyIndex);
					continue;
				}

				// Early out if this is not a destructible and the body is not simulated.
				if (!OverlapBody->IsInstanceSimulatingPhysics() && !Cast<UDestructibleComponent>(OverlapComp))
				{
					continue;
				}

				FTransform BodyTransform = OverlapBody->GetUnrealWorldTransform();

				const FVector BodyVelocity = OverlapBody->GetUnrealWorldVelocity();
				const FVector BodyLocation = BodyTransform.GetLocation();
				const FVector LineTraceEnd = MyLocation + CapsuleDown * ((BodyLocation - MyLocation) | CapsuleDown);

				// Trace to get the hit location on the capsule.
				FHitResult Hit;
				bool bHasHit = UpdatedPrimitive->LineTraceComponent(Hit, BodyLocation, LineTraceEnd, QueryParams);

				FVector HitLoc = Hit.ImpactPoint;
				bool bIsPenetrating = Hit.bStartPenetrating || Hit.PenetrationDepth > StopBodyDistance;

				// If we didn't hit the capsule, we're inside the capsule.
				if (!bHasHit)
				{
					HitLoc = BodyLocation;
					bIsPenetrating = true;
				}

				const float DistanceNow = FVector::VectorPlaneProject(HitLoc - BodyLocation, CapsuleDown).SizeSquared();
				const float DistanceLater = FVector::VectorPlaneProject(HitLoc - (BodyLocation + BodyVelocity * DeltaSeconds), CapsuleDown).SizeSquared();

				if (bHasHit && DistanceNow < StopBodyDistance && !bIsPenetrating)
				{
					OverlapBody->SetLinearVelocity(FVector(0.0f, 0.0f, 0.0f), false);
				}
				else if (DistanceLater <= DistanceNow || bIsPenetrating)
				{
					FVector ForceCenter = MyLocation;

					if (bHasHit)
					{
						ForceCenter += CapsuleDown * ((HitLoc - MyLocation) | CapsuleDown);
					}
					else
					{
						// Get the axis of the capsule bounded by the following two end points.
						const FVector BottomPoint = ForceCenter + CapsuleDown * CapsuleHalfHeight;
						const FVector TopPoint = ForceCenter - CapsuleDown * CapsuleHalfHeight;
						const FVector Segment = TopPoint - BottomPoint;

						// Project the foreign body location on the segment.
						const float Alpha = ((BodyLocation - BottomPoint) | Segment) / Segment.SizeSquared();

						if (Alpha < 0.0f)
						{
							ForceCenter = BottomPoint;
						}
						else if (Alpha > 1.0f)
						{
							ForceCenter = TopPoint;
						}
					}

					OverlapBody->AddRadialForceToBody(ForceCenter, RepulsionForceRadius, RepulsionForce * Mass, ERadialImpulseFalloff::RIF_Constant);
				}
			}
		}
	}
}

void UDashCharacterMovementComponent::ApplyAccumulatedForces(float DeltaSeconds)
{
	if ((!PendingImpulseToApply.IsZero() || !PendingForceToApply.IsZero()) && IsMovingOnGround())
	{
		const FVector Impulse = PendingImpulseToApply + PendingForceToApply * DeltaSeconds + GetGravity() * DeltaSeconds;

		// Check to see if applied momentum is enough to overcome gravity.
		if ((Impulse | GetComponentAxisZ()) > SMALL_NUMBER)
		{
			SetMovementMode(MOVE_Falling);
		}
	}

	Velocity += PendingImpulseToApply + PendingForceToApply * DeltaSeconds;

	PendingImpulseToApply = FVector::ZeroVector;
	PendingForceToApply = FVector::ZeroVector;
}

FVector UDashCharacterMovementComponent::GetGravity() const
{
	if (!CustomGravityDirection.IsZero())
	{
		return CustomGravityDirection * (FMath::Abs(UPawnMovementComponent::GetGravityZ()) * GravityScale);
	}

	if (UpdatedComponent != nullptr && !GravityPoint.IsZero())
	{
		const FVector GravityDir = GravityPoint - UpdatedComponent->GetComponentLocation();
		if (!GravityDir.IsZero())
		{
			return GravityDir.GetSafeNormal() * (FMath::Abs(UPawnMovementComponent::GetGravityZ()) * GravityScale);
		}
	}

	return FVector(0.0f, 0.0f, GetGravityZ());
}

FVector UDashCharacterMovementComponent::GetGravityDirection(bool bAvoidZeroGravity) const
{
	// Gravity direction can be influenced by the custom gravity scale value.
	if (GravityScale != 0.0f)
	{
		if (!CustomGravityDirection.IsZero())
		{
			return CustomGravityDirection * ((GravityScale > 0.0f) ? 1.0f : -1.0f);
		}

		if (UpdatedComponent != nullptr && !GravityPoint.IsZero())
		{
			const FVector GravityDir = GravityPoint - UpdatedComponent->GetComponentLocation();
			if (!bAvoidZeroGravity || !GravityDir.IsZero())
			{
				return GravityDir.GetSafeNormal() * ((GravityScale > 0.0f) ? 1.0f : -1.0f);
			}
		}

		const float WorldGravityZ = UPawnMovementComponent::GetGravityZ();
		if (bAvoidZeroGravity || WorldGravityZ != 0.0f)
		{
			return FVector(0.0f, 0.0f, ((WorldGravityZ > 0.0f) ? 1.0f : -1.0f) * ((GravityScale > 0.0f) ? 1.0f : -1.0f));
		}
	}
	else if (bAvoidZeroGravity)
	{
		if (!CustomGravityDirection.IsZero())
		{
			return CustomGravityDirection;
		}

		if (UpdatedComponent != nullptr && !GravityPoint.IsZero())
		{
			const FVector GravityDir = GravityPoint - UpdatedComponent->GetComponentLocation();
			if (!GravityDir.IsZero())
			{
				return GravityDir.GetSafeNormal();
			}
		}

		return FVector(0.0f, 0.0f, (UPawnMovementComponent::GetGravityZ() > 0.0f) ? 1.0f : -1.0f);
	}

	return FVector::ZeroVector;
}

float UDashCharacterMovementComponent::GetGravityMagnitude() const
{
	return FMath::Abs(GetGravityZ());
}

void UDashCharacterMovementComponent::SetGravityDirection(const FVector& NewGravityDirection)
{
	SetCustomGravityDirection(NewGravityDirection.GetSafeNormal());
}

FORCEINLINE void UDashCharacterMovementComponent::SetCustomGravityDirection(const FVector& NewCustomGravityDirection)
{
	bDirtyCustomGravityDirection = CustomGravityDirection != NewCustomGravityDirection;
	CustomGravityDirection = NewCustomGravityDirection;
}

void UDashCharacterMovementComponent::ClientSetCustomGravityDirection_Implementation(const FVector& NewCustomGravityDirection)
{
	SetCustomGravityDirection(NewCustomGravityDirection);
}

void UDashCharacterMovementComponent::ClientClearCustomGravityDirection_Implementation()
{
	SetCustomGravityDirection(FVector::ZeroVector);
}

void UDashCharacterMovementComponent::ClientSetGravityPoint_Implementation(const FVector& NewGravityPoint)
{
	GravityPoint = NewGravityPoint;
}

void UDashCharacterMovementComponent::ClientClearGravityPoint_Implementation()
{
	GravityPoint = FVector::ZeroVector;
}

void UDashCharacterMovementComponent::ClientSetGravityScale_Implementation(float NewGravityScale)
{
	GravityScale = NewGravityScale;
}

void UDashCharacterMovementComponent::UpdateGravity(float DeltaTime)
{
	if (bAlignCustomGravityToFloor && IsMovingOnGround() && !CurrentFloor.HitResult.ImpactNormal.IsZero())
	{
		// Set the custom gravity direction to reversed floor normal vector.
		SetCustomGravityDirection(CurrentFloor.HitResult.ImpactNormal * -1.0f);
	}

	if (!bDisableGravityReplication && CharacterOwner && CharacterOwner->HasAuthority() && GetNetMode() > NM_Standalone)
	{
		if (bDirtyCustomGravityDirection)
		{
			// Replicate custom gravity direction to clients.
			(!CustomGravityDirection.IsZero()) ? ClientSetCustomGravityDirection(CustomGravityDirection) : ClientClearCustomGravityDirection();
			bDirtyCustomGravityDirection = false;
		}

		if (OldGravityPoint != GravityPoint)
		{
			// Replicate gravity point to clients.
			(!GravityPoint.IsZero()) ? ClientSetGravityPoint(GravityPoint) : ClientClearGravityPoint();
			OldGravityPoint = GravityPoint;
		}

		if (OldGravityScale != GravityScale)
		{
			// Replicate gravity scale to clients.
			ClientSetGravityScale(GravityScale);
			OldGravityScale = GravityScale;
		}
	}

	UpdateComponentRotation();
}

FRotator UDashCharacterMovementComponent::ConstrainComponentRotation(const FRotator& Rotation) const
{
	// Keep current Z rotation axis of capsule, try to keep X axis of rotation.
	return FRotationMatrix::MakeFromZX(GetComponentAxisZ(), Rotation.Vector()).Rotator();
}

FORCEINLINE FVector UDashCharacterMovementComponent::GetComponentAxisX() const
{
	// Fast simplification of FQuat::RotateVector() with FVector(1,0,0).
	const FQuat ComponentRotation = UpdatedComponent->GetComponentQuat();
	const FVector QuatVector(ComponentRotation.X, ComponentRotation.Y, ComponentRotation.Z);

	return FVector(FMath::Square(ComponentRotation.W) - QuatVector.SizeSquared(), ComponentRotation.Z * ComponentRotation.W * 2.0f,
		ComponentRotation.Y * ComponentRotation.W * -2.0f) + QuatVector * (ComponentRotation.X * 2.0f);
}

FORCEINLINE FVector UDashCharacterMovementComponent::GetComponentAxisZ() const
{
	// Fast simplification of FQuat::RotateVector() with FVector(0,0,1).
	const FQuat ComponentRotation = UpdatedComponent->GetComponentQuat();
	const FVector QuatVector(ComponentRotation.X, ComponentRotation.Y, ComponentRotation.Z);

	return FVector(ComponentRotation.Y * ComponentRotation.W * 2.0f, ComponentRotation.X * ComponentRotation.W * -2.0f,
		FMath::Square(ComponentRotation.W) - QuatVector.SizeSquared()) + QuatVector * (ComponentRotation.Z * 2.0f);
}

FVector UDashCharacterMovementComponent::GetComponentDesiredAxisZ() const
{
	if (bAlignComponentToFloor && IsMovingOnGround() && !CurrentFloor.HitResult.ImpactNormal.IsZero())
	{
		// Align character rotation to floor normal vector.
		return CurrentFloor.HitResult.ImpactNormal;
	}

	if (bAlignComponentToGravity)
	{
		return GetGravityDirection(true) * -1.0f;
	}

	return GetComponentAxisZ();
}

void UDashCharacterMovementComponent::UpdateComponentRotation()
{
	if (!HasValidData())
	{
		return;
	}

	const FVector DesiredCapsuleUp = GetComponentDesiredAxisZ();

	// Abort if angle between new and old capsule 'up' axis almost equals to 0 degrees.
	if ((DesiredCapsuleUp | GetComponentAxisZ()) >= THRESH_NORMALS_ARE_PARALLEL)
	{
		return;
	}

	// Take desired Z rotation axis of capsule, try to keep current X rotation axis of capsule.
	const FMatrix RotationMatrix = FRotationMatrix::MakeFromZX(DesiredCapsuleUp, GetComponentAxisX());

	// Intentionally not using MoveUpdatedComponent to bypass constraints.
	UpdatedComponent->MoveComponent(FVector::ZeroVector, RotationMatrix.Rotator(), true);
}
