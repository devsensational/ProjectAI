// Fill out your copyright notice in the Description page of Project Settings.


#include "Npc/CMNpcBase.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Npc/Component/CMNpcComponentBase.h"
#include "Npc/Component/CMNpcShopComponent.h"
#include "Game/CMLocalEventManager.h"
#include "Game/GameInitialization/CMNpcWorldSubsystem.h"

ACMNpcBase::ACMNpcBase()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(FName("Root"));

	CapsuleComponent = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CapsuleComponent"));
	CapsuleComponent->SetupAttachment(RootComponent);
	
	MeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("MeshComponent"));
	MeshComponent->SetupAttachment(RootComponent);

	NpcCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("NpcCameraComponent"));
	NpcCameraComponent->SetupAttachment(RootComponent);
}

void ACMNpcBase::BeginPlay()
{
	// 주의: Super::BeginPlay() 안에서 (블루프린트 등에 미리 붙어 있는) 컴포넌트들의 BeginPlay가 실행되며
	// 이때 RegisterComponent()로 맵에 등록될 수 있다. 따라서 초기화는 반드시 Super 호출 "이전"에 해야
	// 이미 등록된 항목이 지워지지 않는다.
	RegisteredComponentMap.Empty();
	ActiveNpcComponents.Empty();

	Super::BeginPlay();
	
	RegisterToNpcWorldSubsystem();	// 월드 서브시스템에 자신을 등록

	// 에디터에서 지정한 NpcComponents 배열을 기반으로 컴포넌트 생성
	for (const TSubclassOf<UCMNpcComponentBase>& ComponentClass : NpcComponents)
	{
		if (!*ComponentClass)
		{
			continue;
		}

		// 이 NPC를 Outer로 해서 컴포넌트 생성
		UCMNpcComponentBase* NewComp = NewObject<UCMNpcComponentBase>(this, ComponentClass);
		if (!NewComp)
		{
			continue;
		}

		// 액터가 BeginPlay 진행 중이므로 RegisterComponent() 내부에서 컴포넌트의 BeginPlay가 동기적으로 호출되고,
		// 각 컴포넌트는 BeginPlay에서 스스로 핸들러(이 NPC)에 RegisterComponent를 호출한다.
		NewComp->RegisterComponent();

		ActiveNpcComponents.Add(NewComp);
	}
	
	InitializeShopComponent();
}

void ACMNpcBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 대화 도중 파괴되는 경우 UI를 닫고 델리게이트 바인딩을 정리
	EndDialogue();

	// 월드 서브시스템에서 자신을 해제
	UnregisterFromNpcWorldSubsystem();

	Super::EndPlay(EndPlayReason);
}

void ACMNpcBase::RegisterToNpcWorldSubsystem()
{
	if (NpcId.IsNone())
	{
		// ID가 설정되지 않았으면 등록하지 않음
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (UCMNpcWorldSubsystem* NpcSubsystem = World->GetSubsystem<UCMNpcWorldSubsystem>())
	{
		NpcSubsystem->RegisterNpc(NpcId, this);
	}
}

void ACMNpcBase::UnregisterFromNpcWorldSubsystem()
{
	if (NpcId.IsNone())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (UCMNpcWorldSubsystem* NpcSubsystem = World->GetSubsystem<UCMNpcWorldSubsystem>())
	{
		NpcSubsystem->UnregisterNpc(NpcId);
	}
}

void ACMNpcBase::PerformInteract()
{
	// NPC와 상호작용 시 대화 UI를 표시
	StartDialogue();
}

UCMDialogueNode* ACMNpcBase::CreateDialogueNode(TSubclassOf<UCMDialogueNode> NodeClass, const FText& InDialogueText)
{
	if (!*NodeClass)
	{
		NodeClass = UCMDialogueNode::StaticClass();
	}

	UCMDialogueNode* NewNode = NewObject<UCMDialogueNode>(this, NodeClass);
	if (NewNode)
	{
		// 생성 시점에 텍스트 설정
		NewNode->DialogueText = InDialogueText;

		AllDialogueNodes.Add(NewNode);

		if (!RootDialogueNode)
		{
			RootDialogueNode = NewNode;
			CurrentNode = RootDialogueNode;
		}
	}

	return NewNode;
}

UCMDialogueNode* ACMNpcBase::CreateDialogueNodeWithSettings(
	TSubclassOf<UCMDialogueNode> NodeClass,
	UCMDialogueNode* Parent,
	const FText& InDialogueText)
{
	// 기본 CreateDialogueNode를 통해 생성 + 텍스트 설정까지 한 번에 처리
	UCMDialogueNode* NewNode = CreateDialogueNode(NodeClass, InDialogueText);
	if (!NewNode)
	{
		return nullptr;
	}

	if (Parent)
	{
		NewNode->ParentNode = Parent;
		Parent->Children.Add(NewNode);
	}

	return NewNode;
}

UCMDialogueNode* ACMNpcBase::CreateActionNodeWithSettings(
	TSubclassOf<UCMActionNode> NodeClass,
	UCMDialogueNode* Parent,
	const FText& InDialogueText,
	ECMNpcComponentType InActionType)
{
	if (!*NodeClass)
	{
		NodeClass = UCMActionNode::StaticClass();
	}

	UCMActionNode* NewNode = NewObject<UCMActionNode>(this, NodeClass);
	if (!NewNode)
	{
		return nullptr;
	}

	// 액션 타입 설정
	NewNode->ActionType = InActionType;
	NewNode->DialogueText = InDialogueText;

	// 일반 대화 노드와 동일하게 전체 노드 목록에서 관리
	AllDialogueNodes.Add(NewNode);

	if (Parent)
	{
		NewNode->ParentNode = Parent;
		Parent->Children.Add(NewNode);
	}

	return NewNode;
}

bool ACMNpcBase::MoveToChildNodeByIndex(int32 ChildIndex)
{
	if (!CurrentNode)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveToChildNodeByIndex: CurrentNode is null"));
		return false;
	}

	if (!CurrentNode->Children.IsValidIndex(ChildIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveToChildNodeByIndex: Invalid child index %d"), ChildIndex);
		return false;
	}

	UCMDialogueNode* NextNode = CurrentNode->Children[ChildIndex];
	if (!NextNode)
	{
		UE_LOG(LogTemp, Warning, TEXT("MoveToChildNodeByIndex: Child at index %d is null"), ChildIndex);
		return false;
	}

	CurrentNode = NextNode;
	return true;
}

UCMLocalEventManager* ACMNpcBase::GetLocalEventManager()
{
	if (!LocalEventManager)
	{
		if (const UGameInstance* GameInstance = GetGameInstance())
		{
			LocalEventManager = GameInstance->GetSubsystem<UCMLocalEventManager>();
		}
	}

	return LocalEventManager;
}

bool ACMNpcBase::HasActionChild(const UCMDialogueNode* Node)
{
	if (!Node)
	{
		return false;
	}

	for (const UCMDialogueNode* Child : Node->Children)
	{
		if (Cast<UCMActionNode>(Child))
		{
			return true;
		}
	}

	return false;
}

void ACMNpcBase::StartDialogue()
{
	// 이미 대화 중이면 무시 (AddDynamic 중복 바인딩 → ensure 및 핸들러 중복 호출 방지)
	if (bIsInDialogue)
	{
		return;
	}

	if (!RootDialogueNode)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartDialogue: RootDialogueNode is null on %s"), *GetName());
		return;
	}

	UCMLocalEventManager* EventManager = GetLocalEventManager();
	if (!EventManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartDialogue: LocalEventManager not found"));
		return;
	}

	bIsInDialogue = true;
	CurrentNode = RootDialogueNode;

	EventManager->OnSelectedDialogueChoice.AddUniqueDynamic(this, &ACMNpcBase::HandleChoiceSelected);
	EventManager->OnNextDialogueNodeRequested.AddUniqueDynamic(this, &ACMNpcBase::OnNextDialogueNodeRequested);

	// 루트 노드 텍스트 + 루트에 선택지가 있으면 버튼까지 출력
	PrintCurrentDialogueNodeText();
}

void ACMNpcBase::OnNextDialogueNodeRequested()
{
	if (!bIsInDialogue || !LocalEventManager)
	{
		return;
	}

	if (!CurrentNode)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnNextDialogueNodeRequested: CurrentNode is null"));
		return;
	}

	// 선택지(액션 노드)가 표시된 상태라면 '다음'으로 진행하지 않고 버튼 선택을 기다림
	if (HasActionChild(CurrentNode))
	{
		return;
	}

	const int32 NumChildren = CurrentNode->Children.Num();

	if (NumChildren == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("OnNextDialogueNodeRequested: No more child node. End dialogue."));
		EndDialogue();
		return;
	}

	if (NumChildren != 1)
	{
		UE_LOG(LogTemp, Log, TEXT("OnNextDialogueNodeRequested: Child count is %d (expected 1). No move."), NumChildren);
		return;
	}
	
	UCMDialogueNode* NextNode = CurrentNode->Children[0];
	if (!NextNode)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnNextDialogueNodeRequested: NextNode is null"));
		return;
	}

	CurrentNode = NextNode;
	PrintCurrentDialogueNodeText();
}

void ACMNpcBase::HandleChoiceSelected(ECMNpcComponentType SelectedActionType)
{
	EndDialogue();
	HandleActionByType(SelectedActionType);
}

void ACMNpcBase::EndDialogue()
{
	if (bIsInDialogue)
	{
		// UI 쪽 콜백에서 재진입하더라도 한 번만 처리되도록 먼저 플래그를 내림
		bIsInDialogue = false;

		if (LocalEventManager)
		{
			LocalEventManager->OnEndDialogueNodeRequested.Broadcast();
		}

		UnbindDialogueEvents();
	}

	CurrentNode = RootDialogueNode;
}

void ACMNpcBase::UnbindDialogueEvents()
{
	if (!LocalEventManager)
	{
		return;
	}

	LocalEventManager->OnSelectedDialogueChoice.RemoveDynamic(this, &ACMNpcBase::HandleChoiceSelected);
	LocalEventManager->OnNextDialogueNodeRequested.RemoveDynamic(this, &ACMNpcBase::OnNextDialogueNodeRequested);
	// NOTE: OnEndDialogueNodeRequested 에는 EndDialogue를 바인딩하지 않는다.
	// EndDialogue가 직접 이 이벤트를 Broadcast 하므로, 바인딩하면 재귀 호출이 발생한다.
}

void ACMNpcBase::PrintCurrentDialogueNodeText()
{
	UCMLocalEventManager* EventManager = GetLocalEventManager();
	if (!EventManager || !CurrentNode)
	{
		return;
	}

	EventManager->OnPrintDialogueNodeText.Broadcast(CurrentNode->DialogueText);

	for (const UCMDialogueNode* ChildNode : CurrentNode->Children)
	{
		if (const UCMActionNode* ActionNode = Cast<UCMActionNode>(ChildNode))
		{
			// 액션 노드의 텍스트와 타입을 전달
			EventManager->OnCreateChoiceButton.Broadcast(ActionNode->DialogueText, ActionNode->ActionType);
		}
	}
}

bool ACMNpcBase::PerformRegisterComponent(ECMNpcComponentType ComponentType, UCMNpcComponentBase* NewComponent)
{
	if (!NewComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("PerformRegisterComponent: NewComponent is null (type %d)."), static_cast<int32>(ComponentType));
		return false;
	}

	if (RegisteredComponentMap.Contains(ComponentType))
	{
		UE_LOG(LogTemp, Warning, TEXT("PerformRegisterComponent: ComponentType %d is already registered."), static_cast<int32>(ComponentType));
		return false;
	}

	RegisteredComponentMap.Add(ComponentType, NewComponent);
	UE_LOG(LogTemp, Log, TEXT("PerformRegisterComponent: Registered ComponentType %d."), static_cast<int32>(ComponentType));
	return true;
}

void ACMNpcBase::HandleActionByType(ECMNpcComponentType ComponentType)
{
	if (UCMNpcComponentBase* Component = RegisteredComponentMap.FindRef(ComponentType))
	{
		Component->PerformAction();
	}
}

bool ACMNpcBase::RegisterComponent(ECMNpcComponentType ComponentType, UCMNpcComponentBase* NewComponent)
{
	return PerformRegisterComponent(ComponentType, NewComponent);
}

void ACMNpcBase::InitializeShopComponent()
{
	UCMNpcShopComponent* ShopComp = Cast<UCMNpcShopComponent>(RegisteredComponentMap.FindRef(ECMNpcComponentType::ShopComponent));

	if (!ShopComp)
	{
		// 등록 타이밍에 의존하지 않도록, 실제 보유 중인 컴포넌트에서 직접 탐색
		ShopComp = FindComponentByClass<UCMNpcShopComponent>();
		if (ShopComp)
		{
			UE_LOG(LogTemp, Warning, TEXT("InitializeShopComponent: ShopComponent exists on %s but is not registered to the handler."), *GetName());
		}
	}

	if (!ShopComp)
	{
		return;	// 상점 기능이 없는 NPC
	}

	TArray<FCMShopItemContent> OutShopItems;
	InitializeShopDataFromTable(OutShopItems);
	ShopComp->SetShopItemContents(OutShopItems);
}

void ACMNpcBase::InitializeShopDataFromTable(TArray<FCMShopItemContent>& OutShopItems) const
{
	OutShopItems.Reset();

	if (!ShopItemDataTable)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACMNpcBase::InitializeShopDataFromTable: ShopItemDataTable is not set on %s"), *GetName());
		return;
	}

	// Row 구조체가 FCMShopItemContent 계열인지 먼저 확인 (Row마다 FindRow 에러 로그가 쏟아지는 것 방지)
	const UScriptStruct* RowStruct = ShopItemDataTable->GetRowStruct();
	if (!RowStruct || !RowStruct->IsChildOf(FCMShopItemContent::StaticStruct()))
	{
		UE_LOG(LogTemp, Warning, TEXT("ACMNpcBase::InitializeShopDataFromTable: %s row struct is not FCMShopItemContent (NPC: %s)"),
			*ShopItemDataTable->GetName(), *GetName());
		return;
	}

	const TArray<FName> RowNames = ShopItemDataTable->GetRowNames();
	OutShopItems.Reserve(RowNames.Num());

	for (const FName& RowName : RowNames)
	{
		if (const FCMShopItemContent* Row = ShopItemDataTable->FindRow<FCMShopItemContent>(RowName, TEXT("InitializeShopDataFromTable")))
		{
			OutShopItems.Add(*Row);
		}
	}
}

void ACMNpcBase::Interact_Implementation(AActor* Interactor)
{
	// 플레이어가 이 NPC와 상호작용했을 때 호출
	PerformInteract();
}

FInteractionUIData ACMNpcBase::GetInteractableData_Implementation()
{
	FInteractionUIData Data;

	Data.ActionName = FText::FromString(TEXT("대화하기"));

	// NOTE: NPC 이름 설정을 위한 방법 필요
	if (!NpcId.IsNone())
	{
		Data.TargetName = FText::FromName(NpcId);
	}
	else
	{
		Data.TargetName = FText::FromString(TEXT("NPC"));
	}

	// 입력 키 텍스트는 UI 쪽에서 교체할 수 있으므로 기본값만 설정
	Data.InputKeyText = FText::FromString(TEXT("2"));

	// 즉시 상호작용 타입으로 설정
	Data.InteractionType = EInteractionType::Instant;
	Data.InteractionDuration = 0.0f;
	Data.CurrentProgress = 0.0f;

	return Data;
}
