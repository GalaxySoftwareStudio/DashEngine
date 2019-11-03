////////////////////////////////////////////////////////////
//
// Copyright (C) 2019 GalaxySoftware Studio
//
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it freely,
// subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented;
//    you must not claim that you wrote the original software.
//    If you use this software in a product, an acknowledgment
//    in the product documentation would be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such,
//    and must not be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//
////////////////////////////////////////////////////////////

#include "DashActorComponent.h"
#include "DashEngine.h"


// Sets default values for this component's properties
UDashActorComponent::UDashActorComponent()
{
	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	PrimaryComponentTick.bCanEverTick = true;

}


// Called when the game starts
void UDashActorComponent::BeginPlay()
{
	Super::BeginPlay();
	
	// ...
	
}


// Called every frame
void UDashActorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// ...
}


//Similar to construction script
void UDashActorComponent::PostInitProperties()
{
	Super::PostInitProperties();


	switch (CWType) {
		case EWorldType::Editor :
		{
			OnConstructed_InEditor();
			break;
		}
		case EWorldType::Game :
		{
			OnConstructed_InGame();
			break;
		}
		case EWorldType::PIE :
		{
			OnConstructed_InGame();
			break;
		}
	};


}

void UDashActorComponent::OnConstructed_InEditor_Implementation()
{
	
}

void UDashActorComponent::OnConstructed_InGame_Implementation()
{

}