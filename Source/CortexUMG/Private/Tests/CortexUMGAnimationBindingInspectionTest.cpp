#include "Misc/AutomationTest.h"
#include "Tests/CortexUMGAnimationBindingTestUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Sections/MovieSceneObjectPropertySection.h"
#include "Tracks/MovieSceneObjectPropertyTrack.h"
#include "Channels/MovieSceneObjectPathChannel.h"
#include "Components/SizeBox.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationBindingInspectCanonicalTest,
    "Cortex.UMG.AnimationBinding.InspectCanonical",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationBindingInspectCanonicalTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationBindingFixture Fixture(*this);
    const TArray<uint8> Before = Fixture.CaptureAllAuthoredState();
    const bool bDirtyBefore = Fixture.Blueprint->GetPackage()->IsDirty();
    const FCortexCommandResult Result = Fixture.Router.Execute(
        TEXT("umg.list_animation_bindings"), Fixture.InspectParams());

    TestTrue(TEXT("Canonical inspection succeeds"), Result.bSuccess);
    if (!Result.bSuccess || !Result.Data.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("Three UMG binding records"),
        Result.Data->GetIntegerField(TEXT("umg_binding_count")), 3);
    TestEqual(TEXT("Three MovieScene binding records"),
        Result.Data->GetIntegerField(TEXT("movie_scene_binding_count")), 3);
    TestTrue(TEXT("Four or more property tracks rather than three bindings"),
        Result.Data->GetIntegerField(TEXT("track_count")) >= 4);

    TestTrue(TEXT("Reads do not change authored state"),
        Before == Fixture.CaptureAllAuthoredState());
    TestEqual(TEXT("Reads preserve dirtiness"),
        Fixture.Blueprint->GetPackage()->IsDirty(), bDirtyBefore);

    // Frame rates: independent numerator and denominator
    const TSharedPtr<FJsonObject>* TickRes = nullptr;
    TestTrue(TEXT("tick_resolution present"), Result.Data->TryGetObjectField(TEXT("tick_resolution"), TickRes));
    if (TickRes && TickRes->IsValid())
    {
        TestEqual(TEXT("tick_resolution numerator"), (*TickRes)->GetIntegerField(TEXT("numerator")), 24000);
        TestEqual(TEXT("tick_resolution denominator"), (*TickRes)->GetIntegerField(TEXT("denominator")), 1);
    }

    const TSharedPtr<FJsonObject>* DispRate = nullptr;
    TestTrue(TEXT("display_rate present"), Result.Data->TryGetObjectField(TEXT("display_rate"), DispRate));
    if (DispRate && DispRate->IsValid())
    {
        TestEqual(TEXT("display_rate numerator"), (*DispRate)->GetIntegerField(TEXT("numerator")), 30000);
        TestEqual(TEXT("display_rate denominator"), (*DispRate)->GetIntegerField(TEXT("denominator")), 1001);
    }

    // Playback range: lower/upper bound value and type
    const TSharedPtr<FJsonObject>* PlaybackRange = nullptr;
    TestTrue(TEXT("playback_range present"), Result.Data->TryGetObjectField(TEXT("playback_range"), PlaybackRange));
    if (PlaybackRange && PlaybackRange->IsValid())
    {
        const TSharedPtr<FJsonObject>* LowerBound = nullptr;
        TestTrue(TEXT("lower_bound present"), (*PlaybackRange)->TryGetObjectField(TEXT("lower_bound"), LowerBound));
        if (LowerBound && LowerBound->IsValid())
        {
            TestEqual(TEXT("lower_bound value"), (*LowerBound)->GetIntegerField(TEXT("value")), 120);
            TestEqual(TEXT("lower_bound type"), (*LowerBound)->GetStringField(TEXT("type")), TEXT("Inclusive"));
        }

        const TSharedPtr<FJsonObject>* UpperBound = nullptr;
        TestTrue(TEXT("upper_bound present"), (*PlaybackRange)->TryGetObjectField(TEXT("upper_bound"), UpperBound));
        if (UpperBound && UpperBound->IsValid())
        {
            TestEqual(TEXT("upper_bound value"), (*UpperBound)->GetIntegerField(TEXT("value")), 720);
            TestEqual(TEXT("upper_bound type"), (*UpperBound)->GetStringField(TEXT("type")), TEXT("Exclusive"));
        }

        double LengthSec = 0.0;
        TestTrue(TEXT("length_seconds present"), (*PlaybackRange)->TryGetNumberField(TEXT("length_seconds"), LengthSec));
        TestTrue(TEXT("length_seconds > 0"), LengthSec > 0.0);
    }

    // Exact selector fields on bindings
    const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
    TestTrue(TEXT("bindings array present"), Result.Data->TryGetArrayField(TEXT("bindings"), Bindings));
    if (Bindings)
    {
        TestEqual(TEXT("bindings count matches 3"), Bindings->Num(), 3);
        for (int32 i = 0; i < Bindings->Num(); ++i)
        {
            TSharedPtr<FJsonObject> B = (*Bindings)[i]->AsObject();
            TestTrue(FString::Printf(TEXT("binding[%d] is object"), i), B.IsValid());
            if (!B.IsValid()) continue;

            TestEqual(FString::Printf(TEXT("binding[%d] index"), i), B->GetIntegerField(TEXT("index")), i);
            TestTrue(FString::Printf(TEXT("binding[%d] has valid binding_guid"), i),
                !B->GetStringField(TEXT("binding_guid")).IsEmpty());
            TestTrue(FString::Printf(TEXT("binding[%d] has widget_name"), i),
                B->HasField(TEXT("widget_name")));
            TestTrue(FString::Printf(TEXT("binding[%d] has slot_widget_name"), i),
                B->HasField(TEXT("slot_widget_name")));
            TestTrue(FString::Printf(TEXT("binding[%d] has is_root_widget"), i),
                B->HasField(TEXT("is_root_widget")));
            TestTrue(FString::Printf(TEXT("binding[%d] target_exists"), i),
                B->GetBoolField(TEXT("target_exists")));
            TestEqual(FString::Printf(TEXT("binding[%d] guid_sharing_count"), i),
                B->GetIntegerField(TEXT("guid_sharing_count")), 1);
            TestTrue(FString::Printf(TEXT("binding[%d] possessable_exists"), i),
                B->GetBoolField(TEXT("possessable_exists")));
            TestTrue(FString::Printf(TEXT("binding[%d] has tracks"), i),
                B->GetArrayField(TEXT("tracks")).Num() >= 1);
        }
    }

    // Fingerprint domain_signature verification
    const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
    TestTrue(TEXT("fingerprint present"), Result.Data->TryGetObjectField(TEXT("fingerprint"), Fingerprint));
    if (Fingerprint && Fingerprint->IsValid())
    {
        const TSharedPtr<FJsonObject>* DomainSig = nullptr;
        TestTrue(TEXT("domain_signature present"), (*Fingerprint)->TryGetObjectField(TEXT("domain_signature"), DomainSig));
        if (DomainSig && DomainSig->IsValid())
        {
            TestEqual(TEXT("domain_signature version"), (*DomainSig)->GetIntegerField(TEXT("version")), 2);
            TestEqual(TEXT("domain_signature scope"), (*DomainSig)->GetStringField(TEXT("scope")), TEXT("umg.animation_binding"));
            TestEqual(TEXT("domain_signature animation_name"), (*DomainSig)->GetStringField(TEXT("animation_name")), TEXT("appearance"));
            TestTrue(TEXT("domain_signature digest is non-empty"), !(*DomainSig)->GetStringField(TEXT("digest")).IsEmpty());
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationBindingInspectPaginationTest,
    "Cortex.UMG.AnimationBinding.InspectPagination",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationBindingInspectPaginationTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationBindingFixture Fixture(*this);

    // Page 1: offset 0, limit 1
    TSharedPtr<FJsonObject> P1 = Fixture.InspectParams();
    P1->SetNumberField(TEXT("offset"), 0);
    P1->SetNumberField(TEXT("limit"), 1);
    FCortexCommandResult R1 = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P1);
    TestTrue(TEXT("Page 1 succeeds"), R1.bSuccess);
    if (R1.bSuccess && R1.Data.IsValid())
    {
        const TSharedPtr<FJsonObject>* Pag1 = nullptr;
        if (R1.Data->TryGetObjectField(TEXT("pagination"), Pag1) && Pag1 && Pag1->IsValid())
        {
            TestEqual(TEXT("Page 1 total"), (*Pag1)->GetIntegerField(TEXT("total")), 3);
            TestEqual(TEXT("Page 1 offset"), (*Pag1)->GetIntegerField(TEXT("offset")), 0);
            TestEqual(TEXT("Page 1 limit"), (*Pag1)->GetIntegerField(TEXT("limit")), 1);
            TestEqual(TEXT("Page 1 returned"), (*Pag1)->GetIntegerField(TEXT("returned")), 1);
            TestEqual(TEXT("Page 1 next_offset"), (*Pag1)->GetIntegerField(TEXT("next_offset")), 1);
            TestFalse(TEXT("Page 1 is_complete"), (*Pag1)->GetBoolField(TEXT("is_complete")));
        }
        TestEqual(TEXT("Page 1 bindings returned count"), R1.Data->GetArrayField(TEXT("bindings")).Num(), 1);
    }

    // Page 2: offset 2, limit 2
    TSharedPtr<FJsonObject> P2 = Fixture.InspectParams();
    P2->SetNumberField(TEXT("offset"), 2);
    P2->SetNumberField(TEXT("limit"), 2);
    if (R1.bSuccess && R1.Data.IsValid())
    {
        P2->SetObjectField(TEXT("expected_fingerprint"), R1.Data->GetObjectField(TEXT("fingerprint")));
    }
    FCortexCommandResult R2 = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P2);
    TestTrue(TEXT("Page 2 succeeds"), R2.bSuccess);
    if (R2.bSuccess && R2.Data.IsValid())
    {
        const TSharedPtr<FJsonObject>* Pag2 = nullptr;
        if (R2.Data->TryGetObjectField(TEXT("pagination"), Pag2) && Pag2 && Pag2->IsValid())
        {
            TestEqual(TEXT("Page 2 total"), (*Pag2)->GetIntegerField(TEXT("total")), 3);
            TestEqual(TEXT("Page 2 offset"), (*Pag2)->GetIntegerField(TEXT("offset")), 2);
            TestEqual(TEXT("Page 2 returned"), (*Pag2)->GetIntegerField(TEXT("returned")), 1);
            TestTrue(TEXT("Page 2 next_offset is null"), (*Pag2)->HasTypedField<EJson::Null>(TEXT("next_offset")));
            TestTrue(TEXT("Page 2 is_complete"), (*Pag2)->GetBoolField(TEXT("is_complete")));
        }
    }

    // Page 3: offset 5, limit 10 (beyond total)
    TSharedPtr<FJsonObject> P3 = Fixture.InspectParams();
    P3->SetNumberField(TEXT("offset"), 5);
    P3->SetNumberField(TEXT("limit"), 10);
    if (R1.bSuccess && R1.Data.IsValid())
    {
        P3->SetObjectField(TEXT("expected_fingerprint"), R1.Data->GetObjectField(TEXT("fingerprint")));
    }
    FCortexCommandResult R3 = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P3);
    TestTrue(TEXT("Page 3 succeeds"), R3.bSuccess);
    if (R3.bSuccess && R3.Data.IsValid())
    {
        const TSharedPtr<FJsonObject>* Pag3 = nullptr;
        if (R3.Data->TryGetObjectField(TEXT("pagination"), Pag3) && Pag3 && Pag3->IsValid())
        {
            TestEqual(TEXT("Page 3 returned"), (*Pag3)->GetIntegerField(TEXT("returned")), 0);
            TestTrue(TEXT("Page 3 is_complete"), (*Pag3)->GetBoolField(TEXT("is_complete")));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationBindingInspectTableCasesTest,
    "Cortex.UMG.AnimationBinding.InspectTableCases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationBindingInspectTableCasesTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationBindingFixture Fixture(*this);
    UWidgetBlueprint* WBP = Fixture.Blueprint.Get();

    // Case 1: Null MovieScene
    UWidgetAnimation* NullMSAnim = NewObject<UWidgetAnimation>(WBP, TEXT("NullMSAnim"), RF_Transactional);
    NullMSAnim->MovieScene = nullptr;
    WBP->Animations.Add(NullMSAnim);

    TSharedPtr<FJsonObject> NullMSParams = MakeShared<FJsonObject>();
    NullMSParams->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    NullMSParams->SetStringField(TEXT("animation_name"), TEXT("NullMSAnim"));
    FCortexCommandResult NullMSResult = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), NullMSParams);
    TestTrue(TEXT("Null MovieScene succeeds with graceful fallback"), NullMSResult.bSuccess);
    if (NullMSResult.bSuccess && NullMSResult.Data.IsValid())
    {
        TestEqual(TEXT("Null MovieScene movie_scene_binding_count is 0"),
            NullMSResult.Data->GetIntegerField(TEXT("movie_scene_binding_count")), 0);
        TestEqual(TEXT("Null MovieScene track_count is 0"),
            NullMSResult.Data->GetIntegerField(TEXT("track_count")), 0);
        TestTrue(TEXT("Null MovieScene produces diagnostic"),
            NullMSResult.Data->GetArrayField(TEXT("diagnostics")).Num() > 0);
    }

    // Case 2: Absent target widget and slot
    UWidgetAnimation* TableAnim = NewObject<UWidgetAnimation>(WBP, TEXT("TableAnim"), RF_Transactional);
    UMovieScene* TableMS = NewObject<UMovieScene>(TableAnim, TEXT("TableAnim"));
    TableAnim->MovieScene = TableMS;

    FGuid DanglingGuid = FGuid::NewGuid();
    FWidgetAnimationBinding DanglingBinding;
    DanglingBinding.WidgetName = TEXT("NonExistentWidget");
    DanglingBinding.SlotWidgetName = TEXT("NonExistentSlot");
    DanglingBinding.AnimationGuid = DanglingGuid;
    DanglingBinding.bIsRootWidget = false;
    TableAnim->AnimationBindings.Add(DanglingBinding);

    // Case 3: Scene-only binding (in MovieScene but not in AnimationBindings)
    FGuid SceneOnlyGuid = TableMS->AddPossessable(TEXT("SceneOnlyWidget"), USizeBox::StaticClass());

    // Case 4: Identical duplicate records
    FGuid DupGuid = TableMS->AddPossessable(TEXT("BodySizeBox"), USizeBox::StaticClass());
    FWidgetAnimationBinding DupBinding;
    DupBinding.WidgetName = TEXT("BodySizeBox");
    DupBinding.SlotWidgetName = NAME_None;
    DupBinding.AnimationGuid = DupGuid;
    DupBinding.bIsRootWidget = false;
    TableAnim->AnimationBindings.Add(DupBinding);
    TableAnim->AnimationBindings.Add(DupBinding); // identical duplicate

    // Case 5: Shared GUID (two distinct bindings sharing the same GUID)
    FGuid SharedGuid = TableMS->AddPossessable(TEXT("BorderBody"), UBorder::StaticClass());
    FWidgetAnimationBinding SharedB1;
    SharedB1.WidgetName = TEXT("BorderBody");
    SharedB1.SlotWidgetName = NAME_None;
    SharedB1.AnimationGuid = SharedGuid;
    SharedB1.bIsRootWidget = false;
    FWidgetAnimationBinding SharedB2;
    SharedB2.WidgetName = TEXT("BorderBody");
    SharedB2.SlotWidgetName = TEXT("SlotB");
    SharedB2.AnimationGuid = SharedGuid;
    SharedB2.bIsRootWidget = false;
    TableAnim->AnimationBindings.Add(SharedB1);
    TableAnim->AnimationBindings.Add(SharedB2);

    // Case 6: Master/event tracks on TableMS
    TableMS->AddTrack<UMovieSceneEventTrack>();

    WBP->Animations.Add(TableAnim);

    TSharedPtr<FJsonObject> TableParams = MakeShared<FJsonObject>();
    TableParams->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    TableParams->SetStringField(TEXT("animation_name"), TEXT("TableAnim"));
    FCortexCommandResult TableResult = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), TableParams);
    TestTrue(TEXT("Table cases inspection succeeds"), TableResult.bSuccess);
    if (TableResult.bSuccess && TableResult.Data.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
        TableResult.Data->TryGetArrayField(TEXT("bindings"), Bindings);
        TestNotNull(TEXT("TableAnim bindings present"), Bindings);
        if (Bindings && Bindings->Num() >= 5)
        {
            TSharedPtr<FJsonObject> DanglingObj;
            TArray<TSharedPtr<FJsonObject>> SharedObjs;
            for (const TSharedPtr<FJsonValue>& BVal : *Bindings)
            {
                TSharedPtr<FJsonObject> BObj = BVal->AsObject();
                if (BObj.IsValid())
                {
                    if (BObj->GetStringField(TEXT("widget_name")) == TEXT("NonExistentWidget"))
                    {
                        DanglingObj = BObj;
                    }
                    else if (BObj->GetStringField(TEXT("widget_name")) == TEXT("BorderBody"))
                    {
                        SharedObjs.Add(BObj);
                    }
                }
            }

            TestNotNull(TEXT("DanglingObj found"), DanglingObj.Get());
            if (DanglingObj.IsValid())
            {
                TestFalse(TEXT("Dangling target_exists is false"), DanglingObj->GetBoolField(TEXT("target_exists")));
                TestFalse(TEXT("Dangling slot_exists is false"), DanglingObj->GetBoolField(TEXT("slot_exists")));
                TestFalse(TEXT("Dangling possessable_exists is false"), DanglingObj->GetBoolField(TEXT("possessable_exists")));
            }

            // Shared GUID check
            TestTrue(TEXT("Found at least 2 shared BorderBody objects"), SharedObjs.Num() >= 2);
            if (SharedObjs.Num() >= 2)
            {
                TestEqual(TEXT("Shared GUID sharing count 1"), SharedObjs[0]->GetIntegerField(TEXT("guid_sharing_count")), 2);
                TestEqual(TEXT("Shared GUID sharing count 2"), SharedObjs[1]->GetIntegerField(TEXT("guid_sharing_count")), 2);
            }
        }

        // Diagnostics check: should diagnose dangling target, missing possessable, scene-only binding, duplicate records, shared GUID, and master track
        const TArray<TSharedPtr<FJsonValue>>* Diags = nullptr;
        TableResult.Data->TryGetArrayField(TEXT("diagnostics"), Diags);
        TestNotNull(TEXT("diagnostics present"), Diags);
        if (Diags)
        {
            TestTrue(TEXT("Diagnostics array has entries for edge cases"), Diags->Num() >= 4);
        }
    }

    // Case 7: Missing WidgetTree retains existing loader error
    UPackage* NoTreePackage = CreatePackage(TEXT("/Temp/CortexUMGNoTreeTest"));
    UWidgetBlueprint* NoTreeWBP = NewObject<UWidgetBlueprint>(
        NoTreePackage, TEXT("WBP_NoTree"), RF_Public | RF_Standalone | RF_Transactional);
    NoTreeWBP->ParentClass = UUserWidget::StaticClass();
    NoTreeWBP->WidgetTree = nullptr; // deliberately null

    TSharedPtr<FJsonObject> NoTreeParams = MakeShared<FJsonObject>();
    NoTreeParams->SetStringField(TEXT("asset_path"), NoTreeWBP->GetPathName());
    NoTreeParams->SetStringField(TEXT("animation_name"), TEXT("appearance"));
    FCortexCommandResult NoTreeResult = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), NoTreeParams);
    TestFalse(TEXT("Missing WidgetTree fails"), NoTreeResult.bSuccess);
    TestEqual(TEXT("Missing WidgetTree returns BLUEPRINT_NOT_FOUND"),
        NoTreeResult.ErrorCode, CortexErrorCodes::BlueprintNotFound);

    NoTreeWBP->MarkAsGarbage();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationBindingPaginationScalesTest,
    "Cortex.UMG.AnimationBinding.PaginationScales",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationBindingPaginationScalesTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationBindingFixture Fixture(*this);
    UWidgetBlueprint* WBP = Fixture.Blueprint.Get();

    // 1. 3 records: Page-by-page walk and continuation fingerprint
    {
        TSharedPtr<FJsonObject> P1 = Fixture.InspectParams();
        P1->SetNumberField(TEXT("offset"), 0);
        P1->SetNumberField(TEXT("limit"), 1);
        FCortexCommandResult R1 = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P1);
        TestTrue(TEXT("3-rec Page 1 succeeds"), R1.bSuccess);
        TestEqual(TEXT("3-rec Page 1 returned"), R1.Data->GetObjectField(TEXT("pagination"))->GetIntegerField(TEXT("returned")), 1);
        TestEqual(TEXT("3-rec Page 1 next_offset"), R1.Data->GetObjectField(TEXT("pagination"))->GetIntegerField(TEXT("next_offset")), 1);

        TSharedPtr<FJsonObject> FP = R1.Data->GetObjectField(TEXT("fingerprint"));

        // Page 2 with expected_fingerprint continuation
        TSharedPtr<FJsonObject> P2 = Fixture.InspectParams();
        P2->SetNumberField(TEXT("offset"), 1);
        P2->SetNumberField(TEXT("limit"), 1);
        P2->SetObjectField(TEXT("expected_fingerprint"), FP);
        FCortexCommandResult R2 = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P2);
        TestTrue(TEXT("3-rec Page 2 with valid continuation fingerprint succeeds"), R2.bSuccess);
        TestEqual(TEXT("3-rec Page 2 next_offset"), R2.Data->GetObjectField(TEXT("pagination"))->GetIntegerField(TEXT("next_offset")), 2);

        // Page 3 with expected_fingerprint continuation
        TSharedPtr<FJsonObject> P3 = Fixture.InspectParams();
        P3->SetNumberField(TEXT("offset"), 2);
        P3->SetNumberField(TEXT("limit"), 1);
        P3->SetObjectField(TEXT("expected_fingerprint"), FP);
        FCortexCommandResult R3 = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P3);
        TestTrue(TEXT("3-rec Page 3 succeeds"), R3.bSuccess);
        TestTrue(TEXT("3-rec Page 3 is_complete"), R3.Data->GetObjectField(TEXT("pagination"))->GetBoolField(TEXT("is_complete")));
        TestTrue(TEXT("3-rec Page 3 next_offset is null"), R3.Data->GetObjectField(TEXT("pagination"))->HasTypedField<EJson::Null>(TEXT("next_offset")));

        // Stale continuation: modify key, try to read next page with old fingerprint
        Fixture.ChangeRetainedFloatKey(777.0f);
        FCortexCommandResult StaleR = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P2);
        TestFalse(TEXT("Stale continuation fingerprint fails"), StaleR.bSuccess);
        TestEqual(TEXT("Stale continuation returns STALE_PRECONDITION"), StaleR.ErrorCode, CortexErrorCodes::StalePrecondition);
    }

    // 2. 11 records: 3 pages with limit 5, then empty page
    {
        UWidgetAnimation* Anim11 = NewObject<UWidgetAnimation>(WBP, TEXT("Anim11"), RF_Transactional);
        UMovieScene* MS11 = NewObject<UMovieScene>(Anim11, TEXT("Anim11"));
        Anim11->MovieScene = MS11;

        for (int32 i = 0; i < 11; ++i)
        {
            FGuid Guid = MS11->AddPossessable(FString::Printf(TEXT("Widget11_%d"), i), USizeBox::StaticClass());
            FWidgetAnimationBinding B;
            B.WidgetName = FName(*FString::Printf(TEXT("Widget11_%d"), i));
            B.SlotWidgetName = NAME_None;
            B.AnimationGuid = Guid;
            B.bIsRootWidget = false;
            Anim11->AnimationBindings.Add(B);
        }
        WBP->Animations.Add(Anim11);

        int32 TotalRetrieved = 0;
        int32 Offset = 0;
        const int32 Limit = 5;
        bool bComplete = false;

        TSharedPtr<FJsonObject> ExpectedFp11;
        while (!bComplete)
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("asset_path"), WBP->GetPathName());
            P->SetStringField(TEXT("animation_name"), TEXT("Anim11"));
            P->SetNumberField(TEXT("offset"), Offset);
            P->SetNumberField(TEXT("limit"), Limit);
            if (Offset > 0 && ExpectedFp11.IsValid())
            {
                P->SetObjectField(TEXT("expected_fingerprint"), ExpectedFp11);
            }

            FCortexCommandResult R = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P);
            TestTrue(FString::Printf(TEXT("11-rec page at offset %d succeeds"), Offset), R.bSuccess);
            if (!R.bSuccess || !R.Data.IsValid()) break;

            if (R.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")))
            {
                ExpectedFp11 = R.Data->GetObjectField(TEXT("fingerprint"));
            }

            TSharedPtr<FJsonObject> Pag = R.Data->GetObjectField(TEXT("pagination"));
            TestEqual(TEXT("11-rec total is 11"), Pag->GetIntegerField(TEXT("total")), 11);
            int32 Returned = Pag->GetIntegerField(TEXT("returned"));
            TotalRetrieved += Returned;
            bComplete = Pag->GetBoolField(TEXT("is_complete"));

            if (!bComplete)
            {
                Offset = Pag->GetIntegerField(TEXT("next_offset"));
            }
        }

        TestEqual(TEXT("All 11 records retrieved across pages"), TotalRetrieved, 11);

        // Empty page past total
        TSharedPtr<FJsonObject> PEmpty = MakeShared<FJsonObject>();
        PEmpty->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        PEmpty->SetStringField(TEXT("animation_name"), TEXT("Anim11"));
        PEmpty->SetNumberField(TEXT("offset"), 11);
        PEmpty->SetNumberField(TEXT("limit"), 5);
        if (ExpectedFp11.IsValid())
        {
            PEmpty->SetObjectField(TEXT("expected_fingerprint"), ExpectedFp11);
        }
        FCortexCommandResult REmpty = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), PEmpty);
        TestTrue(TEXT("11-rec empty page succeeds"), REmpty.bSuccess);
        if (REmpty.bSuccess && REmpty.Data.IsValid())
        {
            TestEqual(TEXT("11-rec empty page returned 0"),
                REmpty.Data->GetObjectField(TEXT("pagination"))->GetIntegerField(TEXT("returned")), 0);
            TestTrue(TEXT("11-rec empty page is_complete is true"),
                REmpty.Data->GetObjectField(TEXT("pagination"))->GetBoolField(TEXT("is_complete")));
        }
    }

    // 3. 201 records: Paging with limit 200 (max allowed limit)
    {
        UWidgetAnimation* Anim201 = NewObject<UWidgetAnimation>(WBP, TEXT("Anim201"), RF_Transactional);
        UMovieScene* MS201 = NewObject<UMovieScene>(Anim201, TEXT("Anim201"));
        Anim201->MovieScene = MS201;

        for (int32 i = 0; i < 201; ++i)
        {
            FGuid Guid = MS201->AddPossessable(FString::Printf(TEXT("Widget201_%d"), i), USizeBox::StaticClass());
            FWidgetAnimationBinding B;
            B.WidgetName = FName(*FString::Printf(TEXT("Widget201_%d"), i));
            B.SlotWidgetName = NAME_None;
            B.AnimationGuid = Guid;
            B.bIsRootWidget = false;
            Anim201->AnimationBindings.Add(B);
        }
        WBP->Animations.Add(Anim201);

        // Page 1: offset 0, limit 200
        TSharedPtr<FJsonObject> P1 = MakeShared<FJsonObject>();
        P1->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        P1->SetStringField(TEXT("animation_name"), TEXT("Anim201"));
        P1->SetNumberField(TEXT("offset"), 0);
        P1->SetNumberField(TEXT("limit"), 200);

        FCortexCommandResult R1 = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P1);
        TestTrue(TEXT("201-rec Page 1 succeeds"), R1.bSuccess);
        if (R1.bSuccess && R1.Data.IsValid())
        {
            TSharedPtr<FJsonObject> Pag1 = R1.Data->GetObjectField(TEXT("pagination"));
            TestEqual(TEXT("201-rec total is 201"), Pag1->GetIntegerField(TEXT("total")), 201);
            TestEqual(TEXT("201-rec Page 1 returned 200"), Pag1->GetIntegerField(TEXT("returned")), 200);
            TestEqual(TEXT("201-rec Page 1 next_offset is 200"), Pag1->GetIntegerField(TEXT("next_offset")), 200);
            TestFalse(TEXT("201-rec Page 1 is_complete is false"), Pag1->GetBoolField(TEXT("is_complete")));
        }

        // Page 2: offset 200, limit 200
        TSharedPtr<FJsonObject> P2 = MakeShared<FJsonObject>();
        P2->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        P2->SetStringField(TEXT("animation_name"), TEXT("Anim201"));
        P2->SetNumberField(TEXT("offset"), 200);
        P2->SetNumberField(TEXT("limit"), 200);
        if (R1.bSuccess && R1.Data.IsValid())
        {
            P2->SetObjectField(TEXT("expected_fingerprint"), R1.Data->GetObjectField(TEXT("fingerprint")));
        }

        FCortexCommandResult R2 = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P2);
        TestTrue(TEXT("201-rec Page 2 succeeds"), R2.bSuccess);
        if (R2.bSuccess && R2.Data.IsValid())
        {
            TSharedPtr<FJsonObject> Pag2 = R2.Data->GetObjectField(TEXT("pagination"));
            TestEqual(TEXT("201-rec Page 2 returned 1"), Pag2->GetIntegerField(TEXT("returned")), 1);
            TestTrue(TEXT("201-rec Page 2 is_complete is true"), Pag2->GetBoolField(TEXT("is_complete")));
            TestTrue(TEXT("201-rec Page 2 next_offset is null"), Pag2->HasTypedField<EJson::Null>(TEXT("next_offset")));
        }
    }

    // 4. Invalid offset & limit validation
    {
        // Negative offset
        TSharedPtr<FJsonObject> P = Fixture.InspectParams();
        P->SetNumberField(TEXT("offset"), -1);
        FCortexCommandResult R = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P);
        TestFalse(TEXT("Negative offset fails"), R.bSuccess);
        TestEqual(TEXT("Negative offset returns INVALID_FIELD"), R.ErrorCode, CortexErrorCodes::InvalidField);

        // Limit = 0 (< 1)
        P = Fixture.InspectParams();
        P->SetNumberField(TEXT("limit"), 0);
        R = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P);
        TestFalse(TEXT("Limit 0 fails"), R.bSuccess);
        TestEqual(TEXT("Limit 0 returns INVALID_FIELD"), R.ErrorCode, CortexErrorCodes::InvalidField);

        // Limit = 201 (> 200)
        P = Fixture.InspectParams();
        P->SetNumberField(TEXT("limit"), 201);
        R = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P);
        TestFalse(TEXT("Limit 201 fails"), R.bSuccess);
        TestEqual(TEXT("Limit 201 returns INVALID_FIELD"), R.ErrorCode, CortexErrorCodes::InvalidField);

        // Offset as string
        P = Fixture.InspectParams();
        P->SetStringField(TEXT("offset"), TEXT("zero"));
        R = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P);
        TestFalse(TEXT("Offset as string fails"), R.bSuccess);
        TestEqual(TEXT("Offset as string returns INVALID_FIELD"), R.ErrorCode, CortexErrorCodes::InvalidField);

        // Limit as string
        P = Fixture.InspectParams();
        P->SetStringField(TEXT("limit"), TEXT("fifty"));
        R = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P);
        TestFalse(TEXT("Limit as string fails"), R.bSuccess);
        TestEqual(TEXT("Limit as string returns INVALID_FIELD"), R.ErrorCode, CortexErrorCodes::InvalidField);

        // expected_fingerprint as string
        P = Fixture.InspectParams();
        P->SetStringField(TEXT("expected_fingerprint"), TEXT("invalid_string"));
        R = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), P);
        TestFalse(TEXT("expected_fingerprint as string fails"), R.bSuccess);
        TestEqual(TEXT("expected_fingerprint as string returns INVALID_FIELD"), R.ErrorCode, CortexErrorCodes::InvalidField);
    }

    // 5. Diagnostic set larger than binding set: pages canonical bindings, not diagnostics
    {
        UWidgetAnimation* DiagAnim = NewObject<UWidgetAnimation>(WBP, TEXT("DiagAnim"), RF_Transactional);
        UMovieScene* DiagMS = NewObject<UMovieScene>(DiagAnim, TEXT("DiagAnim"));
        DiagAnim->MovieScene = DiagMS;

        // 2 bindings
        FGuid G1 = FGuid::NewGuid(); // possessable missing
        FWidgetAnimationBinding B1;
        B1.WidgetName = TEXT("MissingW1");
        B1.SlotWidgetName = TEXT("MissingS1");
        B1.AnimationGuid = G1;
        DiagAnim->AnimationBindings.Add(B1);

        FWidgetAnimationBinding B2;
        B2.WidgetName = TEXT("MissingW2");
        B2.SlotWidgetName = NAME_None;
        B2.AnimationGuid = G1; // shared with G1
        DiagAnim->AnimationBindings.Add(B2);

        // Add master tracks and scene-only possessables to create many diagnostics
        DiagMS->AddTrack<UMovieSceneEventTrack>();
        DiagMS->AddPossessable(TEXT("SceneOnly1"), USizeBox::StaticClass());
        DiagMS->AddPossessable(TEXT("SceneOnly2"), UBorder::StaticClass());

        WBP->Animations.Add(DiagAnim);

        TSharedPtr<FJsonObject> DiagP = MakeShared<FJsonObject>();
        DiagP->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        DiagP->SetStringField(TEXT("animation_name"), TEXT("DiagAnim"));

        FCortexCommandResult DiagR = Fixture.Router.Execute(TEXT("umg.list_animation_bindings"), DiagP);
        TestTrue(TEXT("DiagAnim inspection succeeds"), DiagR.bSuccess);
        if (DiagR.bSuccess && DiagR.Data.IsValid())
        {
            TestEqual(TEXT("Pagination total is 2 (bindings count)"),
                DiagR.Data->GetObjectField(TEXT("pagination"))->GetIntegerField(TEXT("total")), 2);
            TestTrue(TEXT("Diagnostics array is larger than bindings count"),
                DiagR.Data->GetArrayField(TEXT("diagnostics")).Num() > 2);
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Object Property Channel Digest Sensitivity Test (UC-1)
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationBindingObjectPropertyChannelDigestSensitivityTest,
    "Cortex.UMG.AnimationBinding.ObjectPropertyChannelDigestSensitivity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationBindingObjectPropertyChannelDigestSensitivityTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationBindingFixture Fixture(*this);
    UWidgetBlueprint* WBP = Fixture.Blueprint.Get();
    TestNotNull(TEXT("WBP exists"), WBP);
    if (!WBP)
    {
        return false;
    }

    UWidgetAnimation* ObjAnim = NewObject<UWidgetAnimation>(WBP, TEXT("ObjPropAnim"), RF_Transactional);
    UMovieScene* MS = NewObject<UMovieScene>(ObjAnim, TEXT("ObjPropMS"));
    ObjAnim->MovieScene = MS;

    const FGuid PGuid = MS->AddPossessable(TEXT("BodySizeBox"), USizeBox::StaticClass());
    FWidgetAnimationBinding Binding;
    Binding.WidgetName = TEXT("BodySizeBox");
    Binding.AnimationGuid = PGuid;
    ObjAnim->AnimationBindings.Add(Binding);

    UMovieSceneObjectPropertyTrack* ObjTrack = MS->AddTrack<UMovieSceneObjectPropertyTrack>(PGuid);
    TestNotNull(TEXT("ObjTrack created"), ObjTrack);
    if (!ObjTrack)
    {
        return false;
    }

    UMovieSceneObjectPropertySection* ObjSec = Cast<UMovieSceneObjectPropertySection>(ObjTrack->CreateNewSection());
    TestNotNull(TEXT("ObjSec created"), ObjSec);
    if (!ObjSec)
    {
        return false;
    }
    ObjTrack->AddSection(*ObjSec);
    ObjSec->SetRange(TRange<FFrameNumber>::Inclusive(0, 1000));

    // Initial state: Key at frame 100 pointing to WBP
    ObjSec->ObjectChannel.GetData().AddKey(FFrameNumber(100), FMovieSceneObjectPathChannelKeyValue(WBP));

    const FString Digest1 = CortexUMGAnimationBindingUtils::ComputeAnimationDigest(
        WBP->GetPathName(), TEXT("ObjPropAnim"), ObjAnim, WBP);
    TestFalse(TEXT("Digest1 is not empty"), Digest1.IsEmpty());

    // Modify: Change the referenced object at frame 100 to WBP->GeneratedClass (same frame, different object)
    ObjSec->ObjectChannel.GetData().GetValues()[0] = FMovieSceneObjectPathChannelKeyValue(WBP->GeneratedClass);

    const FString Digest2 = CortexUMGAnimationBindingUtils::ComputeAnimationDigest(
        WBP->GetPathName(), TEXT("ObjPropAnim"), ObjAnim, WBP);
    TestFalse(TEXT("Digest2 is not empty"), Digest2.IsEmpty());
    TestNotEqual(TEXT("Digest changes when object-property key value changes at same frame (UC-1)"), Digest1, Digest2);

    // Modify: Change the default object on the channel
    ObjSec->ObjectChannel.SetDefault(WBP);

    const FString Digest3 = CortexUMGAnimationBindingUtils::ComputeAnimationDigest(
        WBP->GetPathName(), TEXT("ObjPropAnim"), ObjAnim, WBP);
    TestFalse(TEXT("Digest3 is not empty"), Digest3.IsEmpty());
    TestNotEqual(TEXT("Digest changes when object-property default value changes (UC-1)"), Digest2, Digest3);

    return true;
}

