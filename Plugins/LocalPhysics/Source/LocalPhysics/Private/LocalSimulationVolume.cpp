// Fill out your copyright notice in the Description page of Project Settings.

// local includes for Local Sim Plugin
#include "LocalPhysicsActor.h"
#include "LocalSimulationVolume.h"
#include "LocalPhysicsSimulation.h"
#include "LocalPhysicsActorHandle.h"
#include "LocalPhysicsJointHandle.h"
// includes for global functions
#include "EngineUtils.h"
#include "EngineGlobals.h"
#include "HAL/IConsoleManager.h"
#include "Object.h"
#include "DrawDebugHelpers.h"
#include "Private/KismetTraceUtils.h"
#include "Engine/Engine.h"
#include "SceneManagement.h"
#include "Logging/TokenizedMessage.h"
// includes for components
#include "Components/BoxComponent.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
// includes for physixs
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsPublic.h"

using namespace LocalPhysics;

/* 
 * Constructors 
 */

// Sets default values
ALocalSimulationVolume::ALocalSimulationVolume()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
	//PrimaryActorTick.TickGroup = ETickingGroup::TG_PrePhysics;

	delete LocalSpace;
	LocalSpace = CreateDefaultSubobject<UBoxComponent>(TEXT("LocalSpace"));

	delete LocalSimulation;
	LocalSimulation = new FLocalSimulation();
}

ALocalSimulationVolume::~ALocalSimulationVolume()
{
	for (LocalPhysicJointData* data : JointActors)
	{
		auto temp = data;

		//todo: LocalSimulation->RemoveJoint(temp->JointHandle);

		delete temp;
	}
	for(LocalPhysicData* data : SimulatedActors)
	{
		auto temp = data;

		LocalSimulation->RemoveActor(temp->InHandle);

		temp->InHandle = nullptr;

		temp->InPhysicsMesh = nullptr;
		temp->InVisualMesh = nullptr;
		delete temp;
	}
	SimulatedActors.Empty();
	JointActors.Empty();
	delete LocalSimulation;
}

// Called when the game starts or when spawned
void ALocalSimulationVolume::BeginPlay()
{
	Super::BeginPlay();	
	LocalSpace->RegisterComponent();
}

/*
* Transform Updates
*/

void ALocalSimulationVolume::PollPhysicsUpdate()
{

	/*
	*  any polling work physics -> real world update
	*/
	// dynamic/static pass, kinematic is TransformedUpdated below
	for (LocalPhysicData* MeshData : SimulatedActors)
	{
		// dereference pointers to pointers, and set references
		UStaticMeshComponent& Mesh = *MeshData->InVisualMesh;
		LocalPhysics::FActorHandle& Handle = *MeshData->InHandle;

		FTransform BodyTransform = (Handle.GetWorldTransform() * LocalSpace->ComponentToWorld);

		switch(MeshData->InBodyType)
		{
		case LocalPhysicsBodyType::Static:
		case LocalPhysicsBodyType::Dynamic:
			// update meshes back in 'world' space
			Mesh.SetWorldLocation(BodyTransform.GetLocation(), false, nullptr, ETeleportType::TeleportPhysics);
			Mesh.SetWorldRotation(BodyTransform.GetRotation().Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
			break;
		case LocalPhysicsBodyType::Kinematic:
			// if we are kinematic, we poll updates back into space
			Handle.SetWorldTransform(Mesh.ComponentToWorld.GetRelativeTransform(LocalSpace->ComponentToWorld));
			break;
		}
		// let's show everything in simulation.
		if (showDebugLocalPhysics)
		{
			UKismetSystemLibrary::DrawDebugBox(GetWorld(), Handle.GetWorldTransform().GetLocation(), Mesh.Bounds.GetBox().GetExtent(), DebugSimulatedColor, Handle.GetWorldTransform().GetRotation().Rotator(), DebugTick, DebugThickness);
		}
	}
}


// Called every frame
void ALocalSimulationVolume::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// do any early tick removals
	DeferredRemoval();
	// todo: update state of dynamic/static objects - i.e. turning on physics or change of mobility will move an static actor to dynamic.
	// simulate
	if(LocalSimulation && LocalSimulation->ShouldSimulate())
	{
		LocalSimulation->Simulate(DeltaTime, LocalRotation.RotateVector(LocalSpace->ComponentToWorld.GetRotation().UnrotateVector(Gravity)));
	}
	// poll updates to visuals
	PollPhysicsUpdate();	
}

//DECLARE_EVENT_ThreeParams(USceneComponent, FTransformUpdated, USceneComponent* /*UpdatedComponent*/, EUpdateTransformFlags /*UpdateTransformFlags*/, ETeleportType /*Teleport*/);
// We listen on Kinematic meshes, as this is called when you SetComponentTransform (and children)
void ALocalSimulationVolume::TransformUpdated(USceneComponent* InRootComponent, EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	// presumably, this isn't a physics update. - we want to recieve updates from SetComponentTransform
	if(Teleport == ETeleportType::None && InRootComponent->Mobility.GetValue() == EComponentMobility::Movable)
	{
		// Verify this is a Mesh first.
		if(InRootComponent->IsA(UStaticMeshComponent::StaticClass()))
		{
			// We know what it is, but we need access now.
			UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(InRootComponent);

			if(LocalPhysicData* MeshData = GetDataForStaticMesh(Mesh))
			{
				if(MeshData != nullptr)
				{
					// create easy reference for later
					LocalPhysics::FActorHandle* Handle = MeshData->InHandle;

					// Kinematic update for our physics in 'local' space
					Handle->SetWorldTransform(Mesh->BodyInstance.GetUnrealWorldTransform_AssumesLocked().GetRelativeTransform(LocalSpace->ComponentToWorld));

					const FTransform BodyTransform = (Handle->GetWorldTransform());

					// let's show everything in simulation.
					if (showDebugLocalPhysics)
						UKismetSystemLibrary::DrawDebugBox(GetWorld(), BodyTransform.GetLocation(), Mesh->Bounds.GetBox().GetExtent(), DebugKinematicColor, BodyTransform.GetRotation().Rotator(), DebugTick, DebugKinematicThickness);

					UE_LOG(LogTemp, Warning, TEXT("Getting transfofrm updates."))
				}
			}
		}
	}
}

// addition

bool ALocalSimulationVolume::AddStaticMeshToSimulation(UStaticMeshComponent* Mesh, bool ShouldExistInBothScenes)
{
	bool addResults = false;
	// if we don't find this mesh in Simulated or Kinematic arrays
	if (!InSimulation(Mesh))
	{
		/*
		 * messy check for static, kinematic, dynamic
		 */
		 // default is Dynamic, other checks will override this default if they're true.
		LocalPhysicsBodyType typeOfAdd = LocalPhysicsBodyType::Dynamic;
		// check if Kinematic by Component Mobility == Movable && Physics active
		typeOfAdd = Mesh->Mobility.GetValue() == EComponentMobility::Movable && (!Mesh->IsSimulatingPhysics() || !Mesh->BodyInstance.bSimulatePhysics) ? LocalPhysicsBodyType::Kinematic : typeOfAdd;
		// check if Static by Component Mobility == Static
		typeOfAdd = Mesh->Mobility.GetValue() == EComponentMobility::Static ? LocalPhysicsBodyType::Static : typeOfAdd;

		// init SimulatedActors data
		LocalPhysicData* NewMeshData = new LocalPhysicData(*LocalSimulation, Mesh, ShouldExistInBothScenes ? NewObject<UStaticMeshComponent>(this) : nullptr, nullptr, typeOfAdd);

		// create reference to body instance of mesh being added
		FBodyInstance& BodyInstance = Mesh->BodyInstance;

		// create copy of new relative transform
		const FTransform BodyTransform = Mesh->BodyInstance.GetUnrealWorldTransform_AssumesLocked().GetRelativeTransform(LocalSpace->ComponentToWorld);

		UStaticMeshComponent* DynamicMesh = NewMeshData->InPhysicsMesh;
		if(ShouldExistInBothScenes)
		{
			// by default, we create clones for kinematic components.
			DynamicMesh = NewMeshData->InPhysicsMesh;
			DynamicMesh->SetMobility(EComponentMobility::Movable);
			DynamicMesh->RegisterComponentWithWorld(GetWorld());
			DynamicMesh->SetHiddenInGame(true);
			DynamicMesh->SetMobility(EComponentMobility::Movable);
			DynamicMesh->SetStaticMesh(Mesh->GetStaticMesh());
		}
		else
		{
			DynamicMesh = NewMeshData->InVisualMesh;
		}

		Mesh->SetMobility(EComponentMobility::Movable);

		switch (typeOfAdd)
		{
		case LocalPhysicsBodyType::Kinematic:
			// we are going to listen for transform updates from SetComponentTransform (from the original owner)
			// I want to say this is still necessary for any updates we get in-between this Actors tick cycle.
			Mesh->TransformUpdated.AddUObject(this, &ALocalSimulationVolume::TransformUpdated);
			KinematicActive++;
			NewMeshData->InHandle = LocalSimulation->CreateKinematicActor(BodyInstance.GetPxRigidBody_AssumesLocked(), BodyTransform);
			break;
		case LocalPhysicsBodyType::Static:
			// add new mesh into simulation 'local' space
			StaticBodies++;
			NewMeshData->InHandle = LocalSimulation->CreateKinematicActor(BodyInstance.GetPxRigidBody_AssumesLocked(), BodyTransform);
			break;
		case LocalPhysicsBodyType::Dynamic:
			{
				DynamicActive++;
				// preserve linear / angular velocity for 'local' simulating mesh
				FVector LinearVelocity = Mesh->GetPhysicsLinearVelocity();
				FVector AngularVelocity = Mesh->GetPhysicsAngularVelocity();
				// restore linear / angular velocity to 'local' handle of simulating mesh (newHandle), and convert it from 'world' to 'local' space base on this actor transform.
				// by default, we create clones for kinematic components.

				// add new mesh into simulation 'local' space
				DynamicMesh->SetSimulatePhysics(false);

				// create dynamic rigidbody, which is expected to simulate.
				NewMeshData->InHandle = LocalSimulation->CreateDynamicActor(BodyInstance.GetPxRigidDynamic_AssumesLocked(), BodyTransform);

				if (convertVelocity)
				{
					NewMeshData->InHandle->SetLinearVelocity(LocalRotation.RotateVector(LocalSpace->ComponentToWorld.GetRotation().UnrotateVector(LinearVelocity)));
					NewMeshData->InHandle->SetAngularVelocity(LocalRotation.RotateVector(LocalSpace->ComponentToWorld.GetRotation().UnrotateVector(AngularVelocity)));
				}
			}
			break;

		}

		if(!ShouldExistInBothScenes)
			BodyInstance.TermBody();

		// remove original body from world-space
		// create new pair in kinematic meshses array (we don't update on tick)
		addResults = (SimulatedActors.Add(NewMeshData) > -1);

		UE_LOG(LogTemp, Warning, TEXT("We added %s as %i"), *Mesh->GetOwner()->GetHumanReadableName(), (int)typeOfAdd);
	}
	return addResults;
}

bool ALocalSimulationVolume::AddConstraintToStaticMeshes(UStaticMeshComponent* MeshOne, UStaticMeshComponent* MeshTwo, int ConstraintProfileIndex)
{

	LocalPhysicData* MeshDataOne = GetDataForStaticMesh(MeshOne);
	LocalPhysicData* MeshDataTwo = GetDataForStaticMesh(MeshTwo);

	FActorHandle* ActorOne = MeshDataOne->InHandle;
	FActorHandle* ActorTwo = MeshDataTwo->InHandle;

	const FConstraintInstance ConstraintProfile = GetConstraintProfile(ConstraintProfileIndex);
	if (MeshDataOne && MeshDataTwo)
	{
		LocalPhysics::LocalPhysicJointData* newData = new LocalPhysicJointData(*LocalSimulation, TArray<LocalPhysics::LocalPhysicData*>{ MeshDataOne, MeshDataTwo }, nullptr, MeshDataOne->InBodyType, MeshDataTwo->InBodyType);
		
		PxD6Joint* PD6Joint = PxD6JointCreate(*GPhysXSDK, nullptr, PxTransform(PxIdentity), nullptr, U2PTransform(ActorTwo->GetWorldTransform().GetRelativeTransform(ActorOne->GetWorldTransform())));
		
		ConstraintProfile.ProfileInstance.UpdatePhysX_AssumesLocked(PD6Joint, (ActorOne->GetInverseMass() + ActorTwo->GetInverseMass() / 2), 1.f);
		
		newData->JointHandle = LocalSimulation->CreateJoint(PD6Joint, ActorOne, ActorTwo);

		JointActors.Add(newData);

		return true;
	}
	return false;
}

// removal

void ALocalSimulationVolume::DeferredRemoval()
{
	if (DeferRemovalOfBodies)
	{
		for (LocalPhysicJointData* Joint : JointsToRemove)
		{
			LocalPhysicJointData* data = Joint;

			LocalSimulation->RemoveJoint(data->JointHandle);

			Joint->Bodies.Empty();

			//todo: delete Joint->JointHandle;
			Joint->JointHandle = nullptr;

			JointsActive--;
		}
		JointsToRemove.Empty();
		for (LocalPhysicData* data : MeshDataToRemove)
		{
			// create pointers to mesh/handle for use later
			LocalPhysicData* temp = data;

			// visual mesh which we want to restore to
			UStaticMeshComponent* mesh = temp->InVisualMesh;

			// prep for reference of handle
			LocalPhysics::FActorHandle* Handle = temp->InHandle;

			// create copy of new position in world
			const FTransform BodyTransform = Handle->GetWorldTransform() * LocalSpace->ComponentToWorld;

			// store pointer to bodyinstance for later
			FBodyInstance* BodyInstance = mesh->GetBodyInstance();

			// If we are null, no doppel created. let's initialize body back in world
			if (temp->InPhysicsMesh == nullptr)
			{
				BodyInstance->TermBody();
				BodyInstance->InitBody(mesh->GetBodySetup(), BodyTransform, mesh, GetWorld()->GetPhysicsScene());
			}

			switch (temp->InBodyType)
			{
			case LocalPhysicsBodyType::Static:
				temp->InVisualMesh->SetMobility(EComponentMobility::Static);
				StaticBodies--;
				break;
			case LocalPhysicsBodyType::Kinematic:
				temp->InVisualMesh->SetMobility(EComponentMobility::Movable);
				KinematicActive--;
				break;
			case LocalPhysicsBodyType::Dynamic:

				// preserve linear / angular velocity for 'local' simulating mesh, and convert it to 'world' space
				FVector LinearVelocity = LocalSpace->ComponentToWorld.GetRotation().RotateVector(LocalRotation.UnrotateVector(Handle->GetLinearVelocity()));
				FVector AngularVelocity = LocalSpace->ComponentToWorld.GetRotation().RotateVector(LocalRotation.UnrotateVector(Handle->GetAngularVelocity()));


				mesh->SetMobility(EComponentMobility::Movable);
				mesh->SetSimulatePhysics(true);
				// restore linear / angular velocity
				if (convertVelocity)
				{
					mesh->SetPhysicsLinearVelocity(LinearVelocity);
					mesh->SetPhysicsAngularVelocity(AngularVelocity);
				}
				DynamicActive--;
				break;
			}
			LocalSimulation->RemoveActor(Handle);
			SimulatedActors.Remove(data);
			delete temp;
		}
		// no matter what, clear until new request
		MeshDataToRemove.Empty();
		DeferRemovalOfBodies = false;
	}
}


bool ALocalSimulationVolume::RemoveConstraintFromStaticMeshes(UStaticMeshComponent* MeshOne, UStaticMeshComponent* MeshTwo)
{
	LocalPhysics::LocalPhysicJointData* JointData = GetDataForJoint(MeshOne, MeshTwo);
	if (JointData != nullptr)
	{
		JointsToRemove.Add(JointData);
		DeferRemovalOfBodies = true;
	}
	return false;
}


bool ALocalSimulationVolume::RemoveStaticMeshFromSimulation(UStaticMeshComponent* Mesh)
{
	if (LocalPhysicData* DataForRemoval = GetDataForStaticMesh(Mesh))
	{
		if (DataForRemoval != nullptr && !MeshDataToRemove.Contains(DataForRemoval))
		{
			MeshDataToRemove.Add(DataForRemoval);
			DeferRemovalOfBodies = true;
			return true;
		}
	}
	return false;
}

// Utilities

FConstraintInstance ALocalSimulationVolume::GetConstraintProfile(int Index) const
{
	// 0 is the minimum we will take.
	Index = FPlatformMath::Max(0, Index);

	if (Index < ConstraintProfiles.Num())
	{
		return ConstraintProfiles[Index];
	}
	else
		return FConstraintInstance();
}

LocalPhysics::LocalPhysicJointData* ALocalSimulationVolume::GetDataForJoint(UStaticMeshComponent* MeshOne, UStaticMeshComponent* MeshTwo) const
{
	LocalPhysics::LocalPhysicData* MeshDataOne = GetDataForStaticMesh(MeshOne);
	LocalPhysics::LocalPhysicData* MeshDataTwo = GetDataForStaticMesh(MeshTwo);

	for (LocalPhysicJointData* Joint : JointActors)
	{
		if (Joint->Bodies.Contains(MeshDataOne) && Joint->Bodies.Contains(MeshDataTwo))
		{
			return Joint;
		}
	}
	return nullptr;
}

LocalPhysicData* ALocalSimulationVolume::GetDataForStaticMesh(UStaticMeshComponent* Mesh) const
{
	for (LocalPhysicData* data : SimulatedActors)
	{
		if (data->InVisualMesh == Mesh)
			return data;
	}
	return nullptr;
}

bool ALocalSimulationVolume::InSimulation(UStaticMeshComponent* Mesh) const
{
	if (bool found = GetDataForStaticMesh(Mesh) != nullptr)
	{
		return found;
	}
	return false;
}
