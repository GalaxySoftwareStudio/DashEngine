#include "QuaternionHandler.h"
#include "GenericPlatformMath.h"
#include "DashEngine.h"
#include "Quat.h"


FRotator UQuaternionHandler::QuatToRotator(FQuat Quaternion)
{
	return Quaternion.Rotator();
}

FQuat UQuaternionHandler::RotatorToQuat(FRotator Rotator)
{
	return FQuat(Rotator);
}

FQuat UQuaternionHandler::QuatFromAngleAndAxis(FVector axis, float angle)
{
	return FQuat(axis, angle);
}

FVector UQuaternionHandler::QuatToEuler(FQuat Quaternion)
{
	return Quaternion.Euler();
}

FQuat UQuaternionHandler::QuatProduct(FQuat A, FQuat B)
{
	return A.operator*=(B);
}