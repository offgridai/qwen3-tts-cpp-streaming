
#include "Lipsync/OffgridAITextVisemePlanner.h"
#include "Lipsync/OffgridAICmudictData.h"

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    static bool IsLetter(TCHAR C)
    {
        return (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z'));
    }

    static bool IsUpperAscii(TCHAR C)
    {
        return C >= TEXT('A') && C <= TEXT('Z');
    }

    static bool IsLowerAscii(TCHAR C)
    {
        return C >= TEXT('a') && C <= TEXT('z');
    }

    static bool IsWordChar(TCHAR C)
    {
        return IsLetter(C) || C == TEXT('\'');
    }

    static bool IsDigitChar(TCHAR C)
    {
        return C >= TEXT('0') && C <= TEXT('9');
    }

    static bool IsVowelChar(TCHAR C)
    {
        C = FChar::ToLower(C);
        return C == TEXT('a') || C == TEXT('e') || C == TEXT('i') || C == TEXT('o') || C == TEXT('u') || C == TEXT('y');
    }

    static int32 CountLetters(const FString& Text)
    {
        int32 Count = 0;
        for (TCHAR C : Text)
        {
            if (IsLetter(C)) { ++Count; }
        }
        return Count;
    }

    static bool ContainsAny(const FString& Text, const TCHAR* Chars)
    {
        for (int32 I = 0; I < Text.Len(); ++I)
        {
            for (const TCHAR* P = Chars; *P; ++P)
            {
                if (Text[I] == *P) { return true; }
            }
        }
        return false;
    }

    static bool IsPhraseBreak(TCHAR C)
    {
        return C == TEXT('.') || C == TEXT(',') || C == TEXT('!') || C == TEXT('?') ||
            C == TEXT(';') || C == TEXT(':') ||
            C == TEXT('-');
    }

    static bool IsHardSentenceBreak(TCHAR C)
    {
        // Launch islands are sentence-level only. Phrases may still be created
        // for commas/semicolons/colons so the timing model can add local breath,
        // but those are not drain/launch island boundaries.
        return C == TEXT('.') || C == TEXT('!') || C == TEXT('?');
    }

    static bool IsTitleCaseWord(const FString& Word)
    {
        if (Word.IsEmpty() || !IsUpperAscii(Word[0]))
        {
            return false;
        }
        bool bHasLower = false;
        for (int32 I = 1; I < Word.Len(); ++I)
        {
            const TCHAR C = Word[I];
            if (!IsLetter(C) && C != TEXT('\''))
            {
                continue;
            }
            if (IsLowerAscii(C))
            {
                bHasLower = true;
                continue;
            }
            if (IsUpperAscii(C))
            {
                return false;
            }
        }
        return bHasLower;
    }

    static EOffgridAITextViseme LegacyVisemeForPose(FName PoseID)
    {
        const FString Pose = PoseID.ToString();
        if (Pose == TEXT("22_MBP")) { return EOffgridAITextViseme::MBP; }
        if (Pose == TEXT("12_Ww-Oo-") || Pose == TEXT("16_Ww-Ew-")) { return EOffgridAITextViseme::WUH; }
        if (Pose == TEXT("20_FV") || Pose == TEXT("24_Tongue_Th") || Pose == TEXT("14_ChJjSh")) { return EOffgridAITextViseme::FVS; }
        if (Pose == TEXT("11_Oo") || Pose == TEXT("09_Oh") || Pose == TEXT("10_Or")) { return EOffgridAITextViseme::OOO; }
        if (Pose == TEXT("03_Ee") || Pose == TEXT("04_Ih") || Pose == TEXT("05_Ay") || Pose == TEXT("06_Eh")) { return EOffgridAITextViseme::EEE; }
        if (Pose == TEXT("07_Aa") || Pose == TEXT("08_Ah") || Pose == TEXT("18_Uh")) { return EOffgridAITextViseme::AAA; }
        return EOffgridAITextViseme::Rest;
    }

    static bool IsVowelPose(FName PoseID)
    {
        const FString Pose = PoseID.ToString();
        return Pose == TEXT("03_Ee") || Pose == TEXT("04_Ih") || Pose == TEXT("05_Ay") || Pose == TEXT("06_Eh") ||
            Pose == TEXT("07_Aa") || Pose == TEXT("08_Ah") || Pose == TEXT("09_Oh") || Pose == TEXT("10_Or") ||
            Pose == TEXT("11_Oo") || Pose == TEXT("12_Ww-Oo-") || Pose == TEXT("16_Ww-Ew-") || Pose == TEXT("18_Uh");
    }

    static bool IsLandmarkPose(FName PoseID)
    {
        const FString Pose = PoseID.ToString();
        return Pose == TEXT("22_MBP") || Pose == TEXT("20_FV") || Pose == TEXT("24_Tongue_Th") || Pose == TEXT("14_ChJjSh") ||
            Pose == TEXT("12_Ww-Oo-") || Pose == TEXT("16_Ww-Ew-");
    }

    struct FWordSpan
    {
        FString Word;
        int32 StartChar = 0;
        int32 EndCharExclusive = 0;
        int32 PhraseIndex = 0;
        int32 SentenceIslandIndex = 0;
        TCHAR BoundaryPunctuationAfter = 0;
    };

    struct FSBVowelAnchor
    {
        FName PoseID = NAME_None;
        float Fraction = 0.0f;
    };

    struct FL1Candidate
    {
        FName PoseID = NAME_None;
        float Fraction = 0.0f;
        float SpanNorm = 0.0f;
        float Strength = 0.0f;
        FString SourceWord;
        int32 WordIndex = INDEX_NONE;
        int32 PhraseIndex = 0;
        int32 SentenceIslandIndex = 0;
        bool bIsLandmark = false;
        bool bIsDominant = false;
        bool bIsFunctionWord = false;
        FName Generator = NAME_None;
    };

    struct FL1TimedCandidate : FL1Candidate
    {
        float StartNorm = 0.0f;
        float EndNorm = 0.0f;
        float CenterNorm = 0.0f;
        int32 PhraseSize = 1;
        float WordSpanStartNorm = 0.0f;
        float WordSpanEndNorm = 0.0f;
        float LocalDensity = 0.0f;
        float WordWindowDensity = 0.0f;
        float TimingBiasApplied = 0.0f;
    };

    struct FSpecialSpec
    {
        FName PoseID;
        float Fraction;
        float SpanNorm;
        float Strength;
    };

    struct FScheduleProfile
    {
        bool bUseScheduled = false;
        float Blend = 0.0f;
        TSet<int32> EvenCadencePhrases;
        bool bShortTailDismissal = false;
        bool bCompactExchangeLine = false;
        bool bShortAdvisoryLine = false;
        bool bPhraseBoundaryLine = false;
        TMap<int32, int32> PhraseSizes;
        bool bHasEvenCadence = false;
    };

    struct FAdaptiveProfile
    {
        float CandidateDensityMean = 0.0f;
        float CandidateDensityPeak = 0.0f;
        bool bIsDenseLine = false;
        int32 DensePhraseCount = 0;
        int32 RoundedClusterCount = 0;
        bool bListLikeLine = false;
    };

    static bool IsTinyFunctionWord(const FString& Word)
    {
        static const TSet<FString> Words = {
            TEXT("a"),TEXT("an"),TEXT("and"),TEXT("at"),TEXT("by"),TEXT("for"),TEXT("if"),TEXT("in"),TEXT("is"),TEXT("it"),TEXT("of"),TEXT("on"),TEXT("or"),TEXT("so"),TEXT("the"),TEXT("to"),TEXT("we"),TEXT("i"),TEXT("me")
        };
        return Words.Contains(Word);
    }

    static bool IsReducedWord(const FString& Word)
    {
        static const TSet<FString> Words = {
            TEXT("can"),TEXT("do"),TEXT("have"),TEXT("has"),TEXT("had"),TEXT("are"),TEXT("was"),TEXT("were"),TEXT("would"),TEXT("could"),TEXT("should"),TEXT("will"),TEXT("let"),TEXT("lets"),TEXT("get"),TEXT("got"),TEXT("see"),TEXT("say"),TEXT("too"),TEXT("but"),TEXT("what"),TEXT("no")
        };
        return IsTinyFunctionWord(Word) || Words.Contains(Word);
    }

    static bool IsMeaningfulWord(const FString& Word)
    {
        return Word.Len() >= 3 && !IsTinyFunctionWord(Word);
    }

    static bool IsCountCadenceWord(const FString& Word)
    {
        static const TSet<FString> Words = { TEXT("one"),TEXT("two"),TEXT("three"),TEXT("four"),TEXT("five"),TEXT("six"),TEXT("seven"),TEXT("eight"),TEXT("nine"),TEXT("ten"),TEXT("alpha"),TEXT("beta"),TEXT("gamma"),TEXT("delta") };
        return Words.Contains(Word);
    }

    static bool IsOrdinalCadenceWord(const FString& Word)
    {
        static const TSet<FString> Words = { TEXT("first"),TEXT("second"),TEXT("third"),TEXT("fourth") };
        return Words.Contains(Word);
    }

    static int32 EstimateSyllableCount(const FString& Word)
    {
        int32 Count = 0;
        bool bWasVowel = false;
        for (int32 I = 0; I < Word.Len(); ++I)
        {
            const bool bNowVowel = IsVowelChar(Word[I]);
            if (bNowVowel && !bWasVowel)
            {
                ++Count;
            }
            bWasVowel = bNowVowel;
        }
        if (Word.Len() > 3 && Word.EndsWith(TEXT("e")) && !Word.EndsWith(TEXT("ee")) && !Word.EndsWith(TEXT("le")) && Count > 1)
        {
            --Count;
        }
        if (Word == TEXT("interesting")) { return 4; }
        if (Word == TEXT("congratulations")) { return 5; }
        if (Word == TEXT("fantastic")) { return 3; }
        if (Word == TEXT("remember")) { return 3; }
        if (Word == TEXT("bodega")) { return 3; }
        if (Word == TEXT("manager")) { return 3; }
        if (Word == TEXT("absolutely")) { return 4; }
        if (Word == TEXT("jalapenos") || Word == TEXT("jalapeños")) { return 4; }
        return FMath::Clamp(Count > 0 ? Count : 1, 1, 6);
    }

    static FName DominantVowelPoseForWord(const FString& Word)
    {
        if (Word == TEXT("do") || Word == TEXT("too") || Word == TEXT("food") || Word == TEXT("good") || Word == TEXT("you")) { return TEXT("11_Oo"); }
        if (Word == TEXT("your") || Word == TEXT("for") || Word == TEXT("or") || Word == TEXT("reporter") || Word == TEXT("colorful")) { return TEXT("10_Or"); }
        if (Word == TEXT("one")) { return TEXT("06_Eh"); }
        if (Word == TEXT("once")) { return TEXT("12_Ww-Oo-"); }
        if (Word == TEXT("got")) { return TEXT("07_Aa"); }
        if (Word == TEXT("some") || Word == TEXT("dont") || Word == TEXT("don't")) { return TEXT("18_Uh"); }
        if (Word == TEXT("coming") || Word == TEXT("lovely") || Word == TEXT("dollars")) { return TEXT("18_Uh"); }
        if (Word == TEXT("no")) { return TEXT("09_Oh"); }
        if (Word.Contains(TEXT("oo")) || Word.Contains(TEXT("ue")) || Word.Contains(TEXT("ui"))) { return TEXT("11_Oo"); }
        if (Word.Contains(TEXT("you")) || Word.Contains(TEXT("ew"))) { return TEXT("16_Ww-Ew-"); }
        if (Word.Contains(TEXT("ow")) || Word.Contains(TEXT("ou"))) { return TEXT("09_Oh"); }
        const int32 AIndex = Word.Find(TEXT("a"));
        if (Word.Len() >= 3 && Word.EndsWith(TEXT("e")) && AIndex >= 0 && AIndex < Word.Len() - 2) { return TEXT("07_Aa"); }
        if (Word.Contains(TEXT("ee")) || Word.Contains(TEXT("ea")) || Word.Contains(TEXT("ie")) || Word.EndsWith(TEXT("y"))) { return TEXT("03_Ee"); }
        if (Word == TEXT("say") || Word == TEXT("says")) { return TEXT("05_Ay"); }
        if (Word.Contains(TEXT("ay")) || Word.Contains(TEXT("ai"))) { return TEXT("05_Ay"); }
        if (Word.Contains(TEXT("e"))) { return TEXT("06_Eh"); }
        if (Word.Contains(TEXT("a"))) { return TEXT("07_Aa"); }
        if (Word.Contains(TEXT("i"))) { return TEXT("04_Ih"); }
        if (Word.EndsWith(TEXT("o")) || Word.Contains(TEXT("o"))) { return TEXT("09_Oh"); }
        if (Word.Contains(TEXT("u"))) { return TEXT("18_Uh"); }
        return TEXT("06_Eh");
    }

    static float WordDurationSeconds(const FString& Word)
    {
        const bool bReduced = IsReducedWord(Word);
        const bool bMeaningful = IsMeaningfulWord(Word);
        const int32 Syllables = EstimateSyllableCount(Word);
        float Duration = bReduced ? 0.088f : (bMeaningful ? 0.176f : 0.118f);
        Duration += static_cast<float>(FMath::Max(Syllables - 1, 0)) * (bMeaningful ? 0.086f : 0.052f);
        if (Word == TEXT("oh") || Word == TEXT("well") || Word == TEXT("alright") || Word == TEXT("wow") || Word == TEXT("hey") || Word == TEXT("ah")) { Duration += 0.075f; }
        if (Word.Len() >= 9 || Syllables >= 4) { Duration += 0.070f; }
        else if (Word.Len() >= 8) { Duration += 0.025f; }
        if (Word == TEXT("santa") || Word == TEXT("matthew") || Word == TEXT("alfie")) { Duration += 0.040f; }
        else if (Word == TEXT("you") || Word == TEXT("your") || Word == TEXT("today")) { Duration += 0.015f; }
        return FMath::Clamp(Duration, 0.070f, 0.620f);
    }

    static TArray<FWordSpan> TokenizeWords(const FString& Text)
    {
        TArray<FWordSpan> Out;
        auto AddNumericWord = [&Out](const FString& Word, int32 Start, int32 EndExclusive, int32 Phrase, int32 SentenceIsland)
        {
            if (Word.IsEmpty())
            {
                return;
            }

            FWordSpan Span;
            Span.Word = Word;
            Span.StartChar = Start;
            Span.EndCharExclusive = EndExclusive;
            Span.PhraseIndex = Phrase;
            Span.SentenceIslandIndex = SentenceIsland;
            Out.Add(Span);
        };

        auto ExpandNumericToken = [&AddNumericWord](const FString& Token, int32 Start, int32 EndExclusive, int32 Phrase, int32 SentenceIsland)
        {
            auto DigitWord = [](TCHAR Digit) -> const TCHAR*
            {
                switch (Digit)
                {
                case TEXT('0'): return TEXT("zero");
                case TEXT('1'): return TEXT("one");
                case TEXT('2'): return TEXT("two");
                case TEXT('3'): return TEXT("three");
                case TEXT('4'): return TEXT("four");
                case TEXT('5'): return TEXT("five");
                case TEXT('6'): return TEXT("six");
                case TEXT('7'): return TEXT("seven");
                case TEXT('8'): return TEXT("eight");
                case TEXT('9'): return TEXT("nine");
                default: return nullptr;
                }
            };

            for (int32 TokenIndex = 0; TokenIndex < Token.Len(); ++TokenIndex)
            {
                const TCHAR C = Token[TokenIndex];
                if (const TCHAR* Word = DigitWord(C))
                {
                    AddNumericWord(FString(Word), Start, EndExclusive, Phrase, SentenceIsland);
                }
                else if ((C == TEXT('.') || C == TEXT(',')) &&
                    TokenIndex > 0 && TokenIndex + 1 < Token.Len() &&
                    IsDigitChar(Token[TokenIndex - 1]) && IsDigitChar(Token[TokenIndex + 1]))
                {
                    AddNumericWord(TEXT("point"), Start, EndExclusive, Phrase, SentenceIsland);
                }
            }
        };

        int32 Phrase = 0;
        int32 SentenceIsland = 0;
        bool bNextWordStartsNewSentenceIsland = false;
        int32 I = 0;
        while (I < Text.Len())
        {
            const TCHAR C = Text[I];
            if (IsDigitChar(C))
            {
                const int32 Start = I;
                FString Token;
                while (I < Text.Len())
                {
                    const TCHAR Nc = Text[I];
                    const bool bJoinNumeric = (Nc == TEXT('.') || Nc == TEXT(',')) &&
                        I > Start && I + 1 < Text.Len() &&
                        IsDigitChar(Text[I - 1]) && IsDigitChar(Text[I + 1]);
                    if (!IsDigitChar(Nc) && !bJoinNumeric)
                    {
                        break;
                    }
                    Token.AppendChar(Nc);
                    ++I;
                }
                if (bNextWordStartsNewSentenceIsland && Out.Num() > 0)
                {
                    ++SentenceIsland;
                    bNextWordStartsNewSentenceIsland = false;
                }
                ExpandNumericToken(Token, Start, I, Phrase, SentenceIsland);
                continue;
            }
            if (IsPhraseBreak(C) || C == TEXT('\n') || C == TEXT('\r'))
            {
                if (IsPhraseBreak(C))
                {
                    if (Out.Num() > 0)
                    {
                        Out.Last().BoundaryPunctuationAfter = C;
                    }
                    ++Phrase;
                }
                if (IsHardSentenceBreak(C))
                {
                    bNextWordStartsNewSentenceIsland = true;
                }
                ++I;
                continue;
            }
            if (!IsWordChar(C))
            {
                ++I;
                continue;
            }
            if (bNextWordStartsNewSentenceIsland && Out.Num() > 0)
            {
                ++SentenceIsland;
                bNextWordStartsNewSentenceIsland = false;
            }

            const int32 Start = I;
            FString Word;
            while (I < Text.Len() && IsWordChar(Text[I]))
            {
                Word.AppendChar(Text[I]);
                ++I;
            }
            FString Clean;
            for (TCHAR Wc : Word)
            {
                if (IsLetter(Wc) || Wc == TEXT('\'')) { Clean.AppendChar(Wc); }
            }
            if (!Clean.IsEmpty())
            {
                FWordSpan Span;
                Span.Word = Clean;
                Span.StartChar = Start;
                Span.EndCharExclusive = I;
                Span.PhraseIndex = Phrase;
                Span.SentenceIslandIndex = SentenceIsland;
                Out.Add(Span);
            }
        }
        return Out;
    }

    static float EstimateDurationSeconds(const FString& Text, const TArray<FWordSpan>& Words)
    {
        if (Words.Num() == 0) { return 0.1f; }
        float Duration = 0.0f;
        int32 LastPhrase = Words[0].PhraseIndex;
        int32 InPhrase = 0;
        int32 Reduced = 0;
        int32 Unusual = 0;
        for (const FWordSpan& W : Words)
        {
            if (W.PhraseIndex != LastPhrase)
            {
                Duration += (InPhrase <= 2) ? 0.105f : 0.075f;
                LastPhrase = W.PhraseIndex;
                InPhrase = 0;
            }
            Duration += WordDurationSeconds(W.Word);
            ++InPhrase;
            Reduced += IsReducedWord(W.Word) ? 1 : 0;
            Unusual += (W.Word.Len() >= 9 || EstimateSyllableCount(W.Word) >= 4) ? 1 : 0;
        }
        Duration += 0.095f;
        for (TCHAR C : Text)
        {
            if (C == TEXT(',') || C == TEXT(';') || C == TEXT(':')) { Duration += 0.045f; }
            else if (C == TEXT('.') || C == TEXT('!') || C == TEXT('?')) { Duration += 0.085f; }
        }
        const float Ratio = static_cast<float>(Reduced) / FMath::Max(1, Words.Num());
        if (Words.Num() <= 9 && Unusual == 0 && Ratio >= 0.38f) { Duration *= 0.78f; }
        else if (Ratio >= 0.46f && Unusual == 0) { Duration *= 0.88f; }
        if (Unusual >= 2) { Duration *= 1.08f; }
        const float Legacy = static_cast<float>(FMath::Max(CountLetters(Text), Text.Len())) / 14.0f;
        const float Preferred = FMath::Max(Duration, Legacy * 0.94f);
        const float Lower = Legacy * (Unusual ? 0.98f : 0.88f);
        const float Upper = Legacy * (Unusual ? 1.78f : 1.48f);
        const float Cap = FMath::Max(Upper, Legacy + (Unusual ? 0.70f : 0.32f));
        return FMath::Max(FMath::Min(FMath::Max3(Preferred, Lower, 0.45f), Cap), 0.1f);
    }

    static void AddSpec(TArray<FSpecialSpec>& Specs, const TCHAR* Pose, float Frac, float Span, float Strength)
    {
        FSpecialSpec Spec;
        Spec.PoseID = FName(Pose);
        Spec.Fraction = Frac;
        Spec.SpanNorm = Span;
        Spec.Strength = Strength;
        Specs.Add(Spec);
    }

    static bool GetSpecialSpecs(const FString& Word, TArray<FSpecialSpec>& Specs)
    {
        Specs.Reset();
        auto S = [&Specs](const TCHAR* Pose, float Frac, float Span, float Strength){ AddSpec(Specs, Pose, Frac, Span, Strength); };
        if (Word == TEXT("hello")) { S(TEXT("06_Eh"),.25f,.34f,.88f); S(TEXT("09_Oh"),.72f,.42f,.96f); return true; }
        if (Word == TEXT("there")) { S(TEXT("24_Tongue_Th"),.16f,.24f,.90f); S(TEXT("06_Eh"),.62f,.58f,.88f); return true; }
        if (Word == TEXT("that")) { S(TEXT("24_Tongue_Th"),.16f,.24f,.90f); S(TEXT("07_Aa"),.62f,.58f,.88f); return true; }
        if (Word == TEXT("the")) { S(TEXT("24_Tongue_Th"),.16f,.20f,.72f); S(TEXT("06_Eh"),.62f,.42f,.62f); return true; }
        if (Word == TEXT("this")) { S(TEXT("24_Tongue_Th"),.16f,.24f,.90f); S(TEXT("06_Eh"),.62f,.58f,.88f); return true; }
        if (Word == TEXT("meet")) { S(TEXT("22_MBP"),.12f,.24f,1.0f); S(TEXT("03_Ee"),.58f,.68f,1.0f); return true; }
        if (Word == TEXT("to") || Word == TEXT("too") || Word == TEXT("two")) { S(TEXT("11_Oo"),.62f,.38f,.94f); return true; }
        if (Word == TEXT("you")) { S(TEXT("11_Oo"),.56f,.72f,1.0f); return true; }
        if (Word == TEXT("your")) { S(TEXT("10_Or"),.60f,.66f,.96f); return true; }
        if (Word == TEXT("i")) { S(TEXT("05_Ay"),.56f,.48f,.90f); return true; }
        if (Word == TEXT("im") || Word == TEXT("i'm")) { S(TEXT("04_Ih"),.30f,.38f,.82f); S(TEXT("22_MBP"),.76f,.24f,.95f); return true; }
        if (Word == TEXT("today")) { S(TEXT("11_Oo"),.34f,.30f,.88f); S(TEXT("05_Ay"),.78f,.42f,.94f); return true; }
        // J7 trajectory templates: compact multi-stage articulations for words
        // that were previously over-collapsed to one representative vowel.
        if (Word == TEXT("about")) { S(TEXT("07_Aa"),.16f,.16f,.62f); S(TEXT("22_MBP"),.36f,.24f,1.0f); S(TEXT("11_Oo"),.72f,.42f,.96f); return true; }
        if (Word == TEXT("once")) { S(TEXT("12_Ww-Oo-"),.16f,.30f,.94f); S(TEXT("18_Uh"),.54f,.36f,.72f); return true; }
        if (Word == TEXT("story")) { S(TEXT("14_ChJjSh"),.12f,.22f,.66f); S(TEXT("10_Or"),.44f,.42f,.92f); S(TEXT("03_Ee"),.78f,.34f,.78f); return true; }
        if (Word == TEXT("say") || Word == TEXT("says")) { S(TEXT("05_Ay"),.62f,.48f,.94f); return true; }
        if (Word == TEXT("got")) { S(TEXT("07_Aa"),.56f,.46f,.88f); return true; }
        if (Word == TEXT("pickles")) { S(TEXT("22_MBP"),.08f,.24f,1.0f); S(TEXT("04_Ih"),.42f,.32f,.58f); S(TEXT("14_ChJjSh"),.78f,.24f,.74f); return true; }
        if (Word == TEXT("thinking")) { S(TEXT("24_Tongue_Th"),.12f,.24f,.78f); S(TEXT("04_Ih"),.38f,.34f,.66f); S(TEXT("06_Eh"),.72f,.36f,.58f); return true; }
        if (Word == TEXT("forgiving")) { S(TEXT("20_FV"),.10f,.24f,.92f); S(TEXT("10_Or"),.30f,.34f,.82f); S(TEXT("04_Ih"),.52f,.26f,.58f); S(TEXT("20_FV"),.68f,.24f,.88f); S(TEXT("04_Ih"),.86f,.28f,.58f); return true; }
        if (Word == TEXT("security")) { S(TEXT("14_ChJjSh"),.10f,.22f,.58f); S(TEXT("06_Eh"),.28f,.30f,.66f); S(TEXT("11_Oo"),.52f,.34f,.70f); S(TEXT("03_Ee"),.82f,.34f,.76f); return true; }
        // J10: long-word trajectory templates. These avoid collapsing multi-syllable
        // content words into a single representative vowel.
        if (Word == TEXT("interesting")) { S(TEXT("04_Ih"),.12f,.24f,.72f); S(TEXT("24_Tongue_Th"),.36f,.22f,.62f); S(TEXT("14_ChJjSh"),.60f,.22f,.72f); S(TEXT("04_Ih"),.84f,.28f,.66f); return true; }
        if (Word == TEXT("interested")) { S(TEXT("04_Ih"),.12f,.24f,.72f); S(TEXT("24_Tongue_Th"),.36f,.22f,.62f); S(TEXT("14_ChJjSh"),.58f,.22f,.70f); S(TEXT("06_Eh"),.82f,.30f,.68f); return true; }
        if (Word == TEXT("companions")) { S(TEXT("22_MBP"),.20f,.24f,.90f); S(TEXT("07_Aa"),.42f,.34f,.78f); S(TEXT("14_ChJjSh"),.68f,.22f,.66f); S(TEXT("18_Uh"),.86f,.28f,.60f); return true; }
        if (Word == TEXT("companion")) { S(TEXT("22_MBP"),.20f,.24f,.90f); S(TEXT("07_Aa"),.44f,.36f,.78f); S(TEXT("14_ChJjSh"),.70f,.22f,.66f); return true; }
        if (Word == TEXT("partial")) { S(TEXT("22_MBP"),.10f,.24f,.94f); S(TEXT("07_Aa"),.36f,.34f,.80f); S(TEXT("14_ChJjSh"),.68f,.22f,.66f); return true; }
        if (Word == TEXT("certain")) { S(TEXT("14_ChJjSh"),.12f,.22f,.62f); S(TEXT("18_Uh"),.40f,.32f,.70f); S(TEXT("06_Eh"),.76f,.30f,.62f); return true; }
        if (Word == TEXT("customer")) { S(TEXT("18_Uh"),.24f,.32f,.68f); S(TEXT("14_ChJjSh"),.52f,.22f,.58f); S(TEXT("22_MBP"),.78f,.24f,.74f); return true; }
        if (Word == TEXT("welcome")) { S(TEXT("12_Ww-Oo-"),.12f,.18f,.92f); S(TEXT("06_Eh"),.44f,.22f,.62f); S(TEXT("07_Aa"),.76f,.18f,.58f); S(TEXT("22_MBP"),.90f,.20f,.98f); return true; }
        if (Word == TEXT("we")) { S(TEXT("12_Ww-Oo-"),.18f,.28f,.84f); return true; }
        if (Word == TEXT("would")) { S(TEXT("12_Ww-Oo-"),.14f,.32f,.84f); return true; }
        if (Word == TEXT("what")) { S(TEXT("12_Ww-Oo-"),.14f,.32f,.84f); return true; }
        // J6: /haʊ/ and /naʊ/ are glide-to-round diphthongs. The open component
        // is a brief lead-in immediately before the dominant Oo target, not an
        // independent salient vowel. Keep the centers close so playback reads as
        // one articulation: quick Aa onset -> sustained Oo.
        if (Word == TEXT("how")) { S(TEXT("07_Aa"),.58f,.16f,.62f); S(TEXT("11_Oo"),.74f,.44f,.98f); return true; }
        if (Word == TEXT("now")) { S(TEXT("07_Aa"),.58f,.16f,.62f); S(TEXT("11_Oo"),.74f,.44f,.98f); return true; }
        if (Word == TEXT("work")) { S(TEXT("12_Ww-Oo-"),.16f,.30f,.82f); return true; }
        if (Word == TEXT("which")) { S(TEXT("12_Ww-Oo-"),.14f,.30f,.84f); S(TEXT("14_ChJjSh"),.72f,.24f,.78f); return true; }
        if (Word == TEXT("but")) { S(TEXT("22_MBP"),.10f,.24f,.80f); return true; }
        if (Word == TEXT("for")) { S(TEXT("20_FV"),.12f,.24f,.82f); S(TEXT("10_Or"),.62f,.44f,.74f); return true; }
        if (Word == TEXT("fresh")) { S(TEXT("20_FV"),.12f,.24f,.84f); S(TEXT("14_ChJjSh"),.56f,.24f,.72f); S(TEXT("06_Eh"),.40f,.42f,.88f); return true; }
        if (Word == TEXT("some")) { S(TEXT("22_MBP"),.18f,.24f,.84f); S(TEXT("06_Eh"),.58f,.44f,.82f); return true; }
        if (Word == TEXT("don't") || Word == TEXT("dont")) { S(TEXT("18_Uh"),.58f,.46f,.84f); return true; }
        if (Word == TEXT("tomatoes")) { S(TEXT("22_MBP"),.20f,.24f,.86f); S(TEXT("07_Aa"),.58f,.52f,.94f); return true; }
        if (Word == TEXT("sandwiches")) { S(TEXT("07_Aa"),.42f,.46f,.90f); S(TEXT("14_ChJjSh"),.74f,.24f,.80f); return true; }
        if (Word == TEXT("couples")) { S(TEXT("22_MBP"),.20f,.24f,.86f); S(TEXT("09_Oh"),.58f,.44f,.90f); return true; }
        if (Word == TEXT("relationships")) { S(TEXT("07_Aa"),.42f,.48f,.92f); S(TEXT("14_ChJjSh"),.66f,.24f,.76f); S(TEXT("22_MBP"),.82f,.24f,.74f); return true; }
        if (Word == TEXT("please")) { S(TEXT("22_MBP"),.10f,.24f,.92f); S(TEXT("03_Ee"),.62f,.46f,.88f); return true; }
        if (Word == TEXT("alfie") || Word == TEXT("alfie's") || Word == TEXT("alfies")) { S(TEXT("07_Aa"),.16f,.30f,.78f); S(TEXT("20_FV"),.54f,.24f,.86f); S(TEXT("03_Ee"),.78f,.42f,.96f); return true; }
        if (Word == TEXT("bodega")) { S(TEXT("22_MBP"),.08f,.24f,1.0f); S(TEXT("09_Oh"),.30f,.28f,.96f); S(TEXT("05_Ay"),.58f,.22f,.74f); S(TEXT("07_Aa"),.82f,.30f,.92f); return true; }
        if (Word == TEXT("manager")) { S(TEXT("22_MBP"),.08f,.24f,.88f); S(TEXT("07_Aa"),.28f,.36f,.94f); S(TEXT("06_Eh"),.70f,.42f,.82f); return true; }
        if (Word == TEXT("matthew")) { S(TEXT("22_MBP"),.10f,.24f,1.0f); S(TEXT("24_Tongue_Th"),.55f,.24f,.86f); S(TEXT("12_Ww-Oo-"),.78f,.36f,.86f); return true; }
        return false;
    }

    static TArray<FSpecialSpec> GenericSpecs(const FString& Word)
    {
        TArray<FSpecialSpec> Specs;
        const bool bMeaningful = IsMeaningfulWord(Word);
        auto S = [&Specs](const TCHAR* Pose, float Frac, float Span, float Strength){ AddSpec(Specs, Pose, Frac, Span, Strength); };
        if (Word.StartsWith(TEXT("th"))) { S(TEXT("24_Tongue_Th"),.12f,.24f,bMeaningful ? .82f : .64f); }
        else if (Word.StartsWith(TEXT("ch")) || Word.StartsWith(TEXT("sh")) || Word.StartsWith(TEXT("j"))) { S(TEXT("14_ChJjSh"),.12f,.24f,bMeaningful ? .78f : .56f); }
        else if (Word.StartsWith(TEXT("s")) || Word.StartsWith(TEXT("z"))) { S(TEXT("14_ChJjSh"),.10f,.22f,bMeaningful ? .64f : .46f); }
        else if (Word.StartsWith(TEXT("wh")) || Word.StartsWith(TEXT("w"))) { S(TEXT("12_Ww-Oo-"),.16f,.34f,bMeaningful ? .88f : .72f); }
        else if (Word.Len() > 0 && (Word[0] == TEXT('m') || Word[0] == TEXT('b') || Word[0] == TEXT('p'))) { S(TEXT("22_MBP"),.10f,.24f,bMeaningful ? .98f : .74f); }
        else if (Word.Len() > 0 && (Word[0] == TEXT('f') || Word[0] == TEXT('v'))) { S(TEXT("20_FV"),.12f,.24f,bMeaningful ? .84f : .66f); }
        const FName Vowel = DominantVowelPoseForWord(Word);
        if (bMeaningful || Word.Len() <= 2 || Vowel == TEXT("09_Oh") || Vowel == TEXT("03_Ee") || Vowel == TEXT("07_Aa"))
        {
            FSpecialSpec Spec;
            Spec.PoseID = Vowel;
            Spec.Fraction = .58f;
            Spec.SpanNorm = bMeaningful ? .58f : .42f;
            Spec.Strength = bMeaningful ? .88f : .62f;
            Specs.Add(Spec);
        }
        if (bMeaningful)
        {
            const TCHAR* MBP = TEXT("mbp");
            int32 Best = INDEX_NONE;
            for (const TCHAR* P = MBP; *P; ++P) { const int32 Idx = Word.Find(FString::Chr(*P)); if (Idx > 0 && (Best == INDEX_NONE || Idx < Best)) { Best = Idx; } }
            if (Best > 0) { S(TEXT("22_MBP"), FMath::Clamp((Best + .5f) / FMath::Max(1, Word.Len()), .08f, .92f), .24f, .80f); }
            Best = INDEX_NONE;
            const TCHAR* FV = TEXT("fv");
            for (const TCHAR* P = FV; *P; ++P) { const int32 Idx = Word.Find(FString::Chr(*P)); if (Idx > 0 && (Best == INDEX_NONE || Idx < Best)) { Best = Idx; } }
            if (Best > 0) { S(TEXT("20_FV"), FMath::Clamp((Best + .5f) / FMath::Max(1, Word.Len()), .08f, .92f), .24f, .70f); }
            const int32 Th = Word.Find(TEXT("th"));
            if (Th > 0) { S(TEXT("24_Tongue_Th"), FMath::Clamp((Th + .5f) / FMath::Max(1, Word.Len()), .08f, .92f), .24f, .70f); }
            int32 Sh = INDEX_NONE;
            const int32 ShIdx = Word.Find(TEXT("sh"));
            if (ShIdx > 0) { Sh = ShIdx; }
            const int32 ChIdx = Word.Find(TEXT("ch"));
            if (ChIdx > 0 && (Sh == INDEX_NONE || ChIdx < Sh)) { Sh = ChIdx; }
            if (Sh > 0) { S(TEXT("14_ChJjSh"), FMath::Clamp((Sh + .6f) / FMath::Max(1, Word.Len()), .10f, .90f), .24f, .68f); }

            // J10 generic long-word fallback: if a meaningful 3+ syllable / 8+
            // letter word still has only one or two events, add a compact internal
            // trajectory. This remains symbolic and does not alter timing ownership.
            const int32 Syllables = EstimateSyllableCount(Word);
            if ((Word.Len() >= 8 || Syllables >= 3) && Specs.Num() < 3)
            {
                const FName DominantPose = DominantVowelPoseForWord(Word);
                bool bHasEarlyVowel = false;
                bool bHasLateVowel = false;
                for (const FSpecialSpec& Existing : Specs)
                {
                    bHasEarlyVowel = bHasEarlyVowel || (Existing.Fraction <= 0.30f && IsVowelPose(Existing.PoseID));
                    bHasLateVowel = bHasLateVowel || (Existing.Fraction >= 0.68f && IsVowelPose(Existing.PoseID));
                }
                if (!bHasEarlyVowel)
                {
                    const FName Early = Word.StartsWith(TEXT("in")) ? FName(TEXT("04_Ih")) : DominantPose;
                    AddSpec(Specs, *Early.ToString(), .20f, .26f, .68f);
                }
                if (Word.Contains(TEXT("st")) || Word.Contains(TEXT("s")))
                {
                    S(TEXT("14_ChJjSh"), .54f, .22f, .60f);
                }
                else if (Word.Contains(TEXT("t")) || Word.Contains(TEXT("d")))
                {
                    S(TEXT("24_Tongue_Th"), .52f, .22f, .54f);
                }
                else if (Word.Contains(TEXT("r")))
                {
                    S(TEXT("10_Or"), .52f, .28f, .58f);
                }
                if (!bHasLateVowel && Specs.Num() < 4)
                {
                    const FName Late = Word.EndsWith(TEXT("ing")) ? FName(TEXT("04_Ih")) : DominantPose;
                    AddSpec(Specs, *Late.ToString(), .82f, .30f, .64f);
                }
            }
        }
        return Specs;
    }

    static TArray<FL1Candidate> GenerateLayer1ACandidates(const TArray<FWordSpan>& Words)
    {
        TArray<FL1Candidate> Candidates;
        for (int32 Wi = 0; Wi < Words.Num(); ++Wi)
        {
            const FString& Word = Words[Wi].Word;
            TArray<FSpecialSpec> Specs;
            const bool bSpecial = GetSpecialSpecs(Word, Specs);
            if (!bSpecial) { Specs = GenericSpecs(Word); }
            const FName Dominant = DominantVowelPoseForWord(Word);
            for (const FSpecialSpec& Spec : Specs)
            {
                FL1Candidate C;
                C.PoseID = Spec.PoseID;
                C.Fraction = Spec.Fraction;
                C.SpanNorm = Spec.SpanNorm;
                C.Strength = Spec.Strength;
                C.SourceWord = Word;
                C.WordIndex = Wi;
                C.PhraseIndex = Words[Wi].PhraseIndex;
                C.SentenceIslandIndex = Words[Wi].SentenceIslandIndex;
                C.bIsLandmark = IsLandmarkPose(Spec.PoseID);
                C.bIsDominant = Spec.PoseID == Dominant;
                C.bIsFunctionWord = IsTinyFunctionWord(Word);
                C.Generator = bSpecial ? TEXT("special") : TEXT("generic");
                Candidates.Add(C);
            }
        }
        return Candidates;
    }

    static FScheduleProfile BuildTimingScheduleProfile(const FString& Text, const TArray<FWordSpan>& Words)
    {
        FScheduleProfile Profile;
        TMap<int32, TArray<FString>> PhraseTokens;
        TMap<int32, TArray<int32>> PhraseSyllables;
        int32 ReducedCount = 0;
        int32 LongContent = 0;
        int32 CommaCount = 0;
        int32 HardBreakCount = 0;
        for (int32 I = 0; I < Words.Num(); ++I)
        {
            const FWordSpan& W = Words[I];
            Profile.PhraseSizes.FindOrAdd(W.PhraseIndex)++;
            PhraseTokens.FindOrAdd(W.PhraseIndex).Add(W.Word);
            PhraseSyllables.FindOrAdd(W.PhraseIndex).Add(EstimateSyllableCount(W.Word));
            ReducedCount += IsReducedWord(W.Word) ? 1 : 0;
            LongContent += (IsMeaningfulWord(W.Word) && EstimateSyllableCount(W.Word) >= 3) ? 1 : 0;
            if (I > 0)
            {
                const FString Sep = Text.Mid(Words[I-1].EndCharExclusive, W.StartChar - Words[I-1].EndCharExclusive);
                CommaCount += ContainsAny(Sep, TEXT(",;:")) ? 1 : 0;
                HardBreakCount += ContainsAny(Sep, TEXT(".!?")) ? 1 : 0;
            }
        }
        for (const TPair<int32, TArray<FString>>& Pair : PhraseTokens)
        {
            const TArray<FString>& Tokens = Pair.Value;
            int32 CadenceWords = 0;
            int32 OrdinalWords = 0;
            int32 GreekWords = 0;
            for (const FString& Token : Tokens)
            {
                CadenceWords += IsCountCadenceWord(Token) ? 1 : 0;
                OrdinalWords += IsOrdinalCadenceWord(Token) ? 1 : 0;
                GreekWords += (Token == TEXT("alpha") || Token == TEXT("beta") || Token == TEXT("gamma") || Token == TEXT("delta")) ? 1 : 0;
            }
            if (CadenceWords >= FMath::Max(2, Tokens.Num() - 1) ||
                (GreekWords >= 1 && CadenceWords + OrdinalWords >= 3) ||
                (GreekWords >= 2 || CadenceWords + OrdinalWords >= FMath::Max(3, Tokens.Num() - 1)))
            {
                Profile.EvenCadencePhrases.Add(Pair.Key);
            }
        }
        const int32 PhraseCount = Profile.PhraseSizes.Num();
        int32 MaxPhraseSize = 0;
        for (const TPair<int32, int32>& Pair : Profile.PhraseSizes) { MaxPhraseSize = FMath::Max(MaxPhraseSize, Pair.Value); }
        const float ReducedRatio = static_cast<float>(ReducedCount) / FMath::Max(1, Words.Num());
        Profile.bShortTailDismissal = PhraseCount == 2 && Words.Num() <= 4 && MaxPhraseSize <= 2;
        Profile.bCompactExchangeLine = PhraseCount == 2 && Words.Num() <= 5 && MaxPhraseSize <= 3;
        Profile.bShortAdvisoryLine = PhraseCount == 1 && Words.Num() <= 7 && ReducedRatio >= 0.35f;
        Profile.bPhraseBoundaryLine = false;
        if (Profile.EvenCadencePhrases.Num() > 0)
        {
            Profile.bUseScheduled = true;
            Profile.Blend = PhraseCount == 1 ? 0.78f : 0.72f;
        }
        else if (Profile.bShortTailDismissal)
        {
            Profile.bUseScheduled = true;
            Profile.Blend = 0.56f;
        }
        else if (Profile.bShortAdvisoryLine)
        {
            Profile.bUseScheduled = true;
            Profile.Blend = 0.34f;
        }
        else if (Profile.bCompactExchangeLine)
        {
            Profile.bUseScheduled = true;
            Profile.Blend = 0.14f;
        }
        else
        {
            int32 MaxSyll = 1;
            for (const TPair<int32, TArray<int32>>& Pair : PhraseSyllables) { for (int32 S : Pair.Value) { MaxSyll = FMath::Max(MaxSyll, S); } }
            if (Words.Num() <= 4 && MaxSyll <= 1)
            {
                Profile.bUseScheduled = true;
                Profile.Blend = 0.56f;
            }
            else if (CommaCount >= 1 && PhraseCount == 1 && Words.Num() <= 10 && LongContent >= 1)
            {
                Profile.bUseScheduled = true;
                Profile.Blend = 0.18f;
            }
        }
        Profile.bHasEvenCadence = Profile.EvenCadencePhrases.Num() > 0;
        return Profile;
    }

    static float TimingPhraseGap(const FString& Text, const FWordSpan& Prev, const FWordSpan& Next, const FScheduleProfile& Profile)
    {
        const FString Sep = Text.Mid(Prev.EndCharExclusive, Next.StartChar - Prev.EndCharExclusive);
        const int32 PrevSize = Profile.PhraseSizes.FindRef(Prev.PhraseIndex) > 0 ? Profile.PhraseSizes.FindRef(Prev.PhraseIndex) : 1;
        const int32 NextSize = Profile.PhraseSizes.FindRef(Next.PhraseIndex) > 0 ? Profile.PhraseSizes.FindRef(Next.PhraseIndex) : 1;
        float Gap = 0.0f;
        if (ContainsAny(Sep, TEXT(".!?")))
        {
            Gap = PrevSize <= 2 ? 0.120f : 0.092f;
            if (Profile.bShortTailDismissal) { Gap += 0.030f; }
            else if (Profile.EvenCadencePhrases.Contains(Prev.PhraseIndex) && NextSize <= 4) { Gap += 0.020f; }
        }
        else if (ContainsAny(Sep, TEXT(",;:")))
        {
            Gap = Profile.EvenCadencePhrases.Contains(Prev.PhraseIndex) ? 0.066f : 0.050f;
        }
        else if (Prev.PhraseIndex != Next.PhraseIndex)
        {
            Gap = PrevSize <= 2 ? 0.112f : 0.082f;
        }
        return Gap;
    }

    static float TimingUnit(const FString& Word, int32 Phrase, int32 PhrasePos, int32 PhraseSize, const FScheduleProfile& Profile)
    {
        const int32 Syll = EstimateSyllableCount(Word);
        if (Profile.EvenCadencePhrases.Contains(Phrase))
        {
            float Unit = 1.0f + 0.05f * FMath::Max(Syll - 1, 0);
            if (IsReducedWord(Word)) { Unit *= 0.94f; }
            return Unit;
        }
        float Unit = WordDurationSeconds(Word);
        if (Profile.bShortTailDismissal && PhraseSize <= 2)
        {
            if (PhrasePos == PhraseSize - 1 && IsMeaningfulWord(Word)) { Unit *= 1.10f; }
            else if (PhrasePos == 0 && IsReducedWord(Word)) { Unit *= 0.92f; }
        }
        else if (Profile.bShortAdvisoryLine)
        {
            if (IsReducedWord(Word)) { Unit *= 0.90f; }
            else if (PhrasePos == PhraseSize - 1 || Syll >= 2) { Unit *= 1.08f; }
        }
        if (Profile.bHasEvenCadence && PhraseSize >= 4 && IsMeaningfulWord(Word) && Syll <= 2)
        {
            Unit *= 1.03f;
        }
        return Unit;
    }

    static TArray<TPair<float, float>> ScheduledWordSpans(const FString& Text, const TArray<FWordSpan>& Words, const FScheduleProfile& Profile)
    {
        TArray<TPair<float, float>> Spans;
        const float CharN = static_cast<float>(FMath::Max(1, Text.Len()));
        if (!Profile.bUseScheduled)
        {
            for (const FWordSpan& W : Words) { Spans.Add(TPair<float,float>(W.StartChar / CharN, W.EndCharExclusive / CharN)); }
            return Spans;
        }
        TArray<TPair<float,float>> Units;
        TMap<int32,int32> PhrasePositions;
        float Total = 0.0f;
        bool bFirst = true;
        FWordSpan Prev;
        for (const FWordSpan& W : Words)
        {
            const int32 PhrasePos = PhrasePositions.FindRef(W.PhraseIndex);
            const int32 PhraseSize = FMath::Max(1, Profile.PhraseSizes.FindRef(W.PhraseIndex));
            if (!bFirst) { Total += TimingPhraseGap(Text, Prev, W, Profile); }
            const float Start = Total;
            Total += TimingUnit(W.Word, W.PhraseIndex, PhrasePos, PhraseSize, Profile);
            Units.Add(TPair<float,float>(Start, Total));
            PhrasePositions.FindOrAdd(W.PhraseIndex) = PhrasePos + 1;
            Prev = W;
            bFirst = false;
        }
        if (Total <= 0.0f)
        {
            for (const FWordSpan& W : Words) { Spans.Add(TPair<float,float>(W.StartChar / CharN, W.EndCharExclusive / CharN)); }
            return Spans;
        }
        for (int32 I = 0; I < Words.Num(); ++I)
        {
            const float CharStart = Words[I].StartChar / CharN;
            const float CharEnd = Words[I].EndCharExclusive / CharN;
            const float DurStart = Units[I].Key / Total;
            const float DurEnd = Units[I].Value / Total;
            const float WStart = Profile.Blend * DurStart + (1.0f - Profile.Blend) * CharStart;
            const float WEnd = Profile.Blend * DurEnd + (1.0f - Profile.Blend) * CharEnd;
            Spans.Add(TPair<float,float>(FMath::Clamp(WStart,0.0f,1.0f), FMath::Clamp(WEnd,0.0f,1.0f)));
        }
        return Spans;
    }

    static float TimingFracAdjust(const FString& Word, FName PoseID, float Frac)
    {
        const FString Pose = PoseID.ToString();
        float Bias = 0.0f;
        if (Pose == TEXT("22_MBP")) { Bias -= 0.03f; }
        else if (Pose == TEXT("24_Tongue_Th")) { Bias -= 0.022f; }
        else if (Pose == TEXT("20_FV")) { Bias -= 0.015f; }
        else if (Pose == TEXT("14_ChJjSh")) { Bias -= 0.015f; }
        else if (Pose == TEXT("11_Oo") || Pose == TEXT("12_Ww-Oo-")) { Bias -= 0.02f; }
        else if (Pose == TEXT("07_Aa")) { Bias -= 0.012f; }
        else if (Pose == TEXT("04_Ih")) { Bias -= 0.015f; }
        else if (Pose == TEXT("10_Or")) { Bias += 0.025f; }
        else if (Pose == TEXT("06_Eh")) { Bias += 0.008f; }
        else if (Pose == TEXT("09_Oh")) { Bias += 0.005f; }
        if (IsReducedWord(Word)) { Bias *= 0.7f; }
        if (Word == TEXT("you") && Pose == TEXT("11_Oo")) { Bias *= 0.45f; }
        else if (Word == TEXT("your") && Pose == TEXT("10_Or")) { Bias *= 0.65f; }
        return FMath::Clamp(Frac + Bias, 0.06f, 0.94f);
    }

    static TArray<FL1TimedCandidate> ApplyLayer1BTimingPrior(const FString& Text, const TArray<FWordSpan>& Words, const TArray<FL1Candidate>& Candidates)
    {
        const FScheduleProfile Profile = BuildTimingScheduleProfile(Text, Words);
        const TArray<TPair<float,float>> Spans = ScheduledWordSpans(Text, Words, Profile);
        TMap<int32,int32> PhraseSizes;
        for (const FWordSpan& W : Words) { PhraseSizes.FindOrAdd(W.PhraseIndex)++; }
        TArray<FL1TimedCandidate> Timed;
        for (const FL1Candidate& C : Candidates)
        {
            if (!Spans.IsValidIndex(C.WordIndex)) { continue; }
            const float WStart = Spans[C.WordIndex].Key;
            const float WEnd = Spans[C.WordIndex].Value;
            const float Frac = TimingFracAdjust(C.SourceWord, C.PoseID, C.Fraction);
            const float Center = WStart + (WEnd - WStart) * Frac;
            const float Width = FMath::Max(0.012f, WEnd - WStart) * C.SpanNorm;
            const float Half = FMath::Max(Width * 0.5f, 0.004f);
            FL1TimedCandidate T;
            static_cast<FL1Candidate&>(T) = C;
            T.StartNorm = FMath::Max(0.0f, Center - Half);
            T.EndNorm = FMath::Min(1.0f, Center + Half);
            T.CenterNorm = (T.StartNorm + T.EndNorm) * 0.5f;
            T.PhraseSize = FMath::Max(1, PhraseSizes.FindRef(C.PhraseIndex));
            T.WordSpanStartNorm = WStart;
            T.WordSpanEndNorm = WEnd;
            T.TimingBiasApplied = Frac - C.Fraction;
            Timed.Add(T);
        }
        TArray<float> Centers;
        for (const FL1TimedCandidate& T : Timed) { Centers.Add(T.CenterNorm); }
        for (FL1TimedCandidate& T : Timed)
        {
            int32 LocalDensity = 0;
            for (float C : Centers) { if (FMath::Abs(C - T.CenterNorm) <= 0.065f) { ++LocalDensity; } }
            T.LocalDensity = static_cast<float>(LocalDensity);
            int32 WordDensity = 0;
            for (const FL1TimedCandidate& Other : Timed) { if (FMath::Abs(Other.WordIndex - T.WordIndex) <= 1) { ++WordDensity; } }
            T.WordWindowDensity = static_cast<float>(WordDensity);
        }
        return Timed;
    }

    static TPair<float,float> DensityStats(const TArray<FL1TimedCandidate>& Events)
    {
        if (Events.Num() == 0) { return TPair<float,float>(0.0f,0.0f); }
        float Sum = 0.0f;
        float Peak = 0.0f;
        for (const FL1TimedCandidate& E : Events)
        {
            Sum += E.LocalDensity;
            Peak = FMath::Max(Peak, E.LocalDensity);
        }
        return TPair<float,float>(Sum / Events.Num(), Peak);
    }

    static void RecomputeLocalDensity(TArray<FL1TimedCandidate>& Events)
    {
        for (FL1TimedCandidate& E : Events)
        {
            int32 Count = 0;
            for (const FL1TimedCandidate& Other : Events) { if (FMath::Abs(Other.CenterNorm - E.CenterNorm) <= 0.065f) { ++Count; } }
            E.LocalDensity = static_cast<float>(Count);
        }
    }

    static FAdaptiveProfile BuildAdaptiveProfile(const TArray<FL1TimedCandidate>& Events)
    {
        FAdaptiveProfile Profile;
        if (Events.Num() == 0) { return Profile; }
        const TPair<float,float> D = DensityStats(Events);
        Profile.CandidateDensityMean = D.Key;
        Profile.CandidateDensityPeak = D.Value;
        TMap<int32, TArray<const FL1TimedCandidate*>> ByPhrase;
        TMap<int32, TArray<const FL1TimedCandidate*>> ByWord;
        for (const FL1TimedCandidate& E : Events)
        {
            ByPhrase.FindOrAdd(E.PhraseIndex).Add(&E);
            ByWord.FindOrAdd(E.WordIndex).Add(&E);
        }
        for (const TPair<int32, TArray<const FL1TimedCandidate*>>& Pair : ByPhrase)
        {
            float Sum = 0.0f;
            float Peak = 0.0f;
            for (const FL1TimedCandidate* E : Pair.Value) { Sum += E->LocalDensity; Peak = FMath::Max(Peak, E->LocalDensity); }
            if (Pair.Value.Num() > 0 && (Sum / Pair.Value.Num() >= 3.5f || Peak >= 6.0f)) { ++Profile.DensePhraseCount; }
        }
        for (const TPair<int32, TArray<const FL1TimedCandidate*>>& Pair : ByWord)
        {
            bool bHasRound = false;
            int32 Vowels = 0;
            for (const FL1TimedCandidate* E : Pair.Value)
            {
                if (E->PoseID == TEXT("12_Ww-Oo-") || E->PoseID == TEXT("11_Oo")) { bHasRound = true; }
                if (IsVowelPose(E->PoseID)) { ++Vowels; }
            }
            if (bHasRound && Vowels >= 2) { ++Profile.RoundedClusterCount; }
        }
        for (const FL1TimedCandidate& E : Events)
        {
            if (E.PhraseSize >= 4 && E.WordWindowDensity >= 5.0f) { Profile.bListLikeLine = true; break; }
        }
        Profile.bIsDenseLine = D.Key >= 2.8f || D.Value >= 5.0f || Events.Num() >= 24 || Profile.DensePhraseCount >= 1;
        return Profile;
    }

    static void IncReason(TMap<FString,int32>& Reasons, const FString& Reason)
    {
        Reasons.FindOrAdd(Reason)++;
    }

    static bool ShouldKeepFunctionVowel(const FString& Word, const FL1TimedCandidate& Vowel, FString& OutReason)
    {
        if (Word == TEXT("a") || Word == TEXT("and") || Word == TEXT("the") || Word == TEXT("to") || Word == TEXT("is") || Word == TEXT("in") || Word == TEXT("on") || Word == TEXT("of") || Word == TEXT("i") || Word == TEXT("me") || Word == TEXT("but"))
        {
            OutReason = TEXT("function_word_vowel_suppressed");
            return false;
        }
        if (Word == TEXT("for") && Vowel.WordWindowDensity >= 7.0f && Vowel.PhraseSize >= 5)
        {
            OutReason = TEXT("dense_function_word_tail");
            return false;
        }
        if (Vowel.Strength <= 0.78f)
        {
            OutReason = TEXT("weak_function_word_vowel");
            return false;
        }
        OutReason.Reset();
        return true;
    }

    static void CompactPhraseFinalTail(TArray<FL1TimedCandidate>& Events, const FAdaptiveProfile& Profile, int32& AdjustmentCount)
    {
        AdjustmentCount = 0;
        if (Events.Num() == 0) { return; }
        int32 LastPhrase = Events[0].PhraseIndex;
        for (const FL1TimedCandidate& E : Events) { LastPhrase = FMath::Max(LastPhrase, E.PhraseIndex); }
        TArray<int32> WordOrder;
        TMap<int32, TArray<int32>> IndicesByWord;
        TArray<float> PhraseCenters;
        for (int32 I = 0; I < Events.Num(); ++I)
        {
            if (Events[I].PhraseIndex != LastPhrase) { continue; }
            IndicesByWord.FindOrAdd(Events[I].WordIndex).Add(I);
            WordOrder.AddUnique(Events[I].WordIndex);
            PhraseCenters.Add(Events[I].CenterNorm);
        }
        if (WordOrder.Num() == 0 || PhraseCenters.Num() == 0) { return; }
        WordOrder.Sort();
        const int32 LastWordIndex = WordOrder.Last();
        TSet<int32> TailWordIndices;
        TailWordIndices.Add(LastWordIndex);
        if (WordOrder.Num() >= 2) { TailWordIndices.Add(WordOrder[WordOrder.Num()-2]); }
        float MinCenter = 1.0f;
        float SumCenter = 0.0f;
        for (float C : PhraseCenters) { MinCenter = FMath::Min(MinCenter, C); SumCenter += C; }
        const float PhraseAvgCenter = SumCenter / PhraseCenters.Num();
        const bool bTailRiskHigh = Profile.bIsDenseLine || Profile.DensePhraseCount >= 2 || PhraseAvgCenter >= 0.78f || MinCenter >= 0.72f;
        const bool bCountStylePhrase = WordOrder.Num() <= 4 && PhraseCenters.Num() <= 6 && Profile.CandidateDensityPeak <= 1.5f;
        for (const TPair<int32, TArray<int32>>& Pair : IndicesByWord)
        {
            TArray<int32> Landmarks;
            TArray<int32> Vowels;
            for (int32 Idx : Pair.Value)
            {
                const FL1TimedCandidate& E = Events[Idx];
                if (E.bIsLandmark) { Landmarks.Add(Idx); }
                if (IsVowelPose(E.PoseID) && !E.bIsLandmark) { Vowels.Add(Idx); }
            }
            if (Vowels.Num() == 0) { continue; }
            if (Landmarks.Num() == 0 && !bCountStylePhrase) { continue; }
            int32 TailIdx = Vowels[0];
            for (int32 Idx : Vowels) { if (Events[Idx].CenterNorm > Events[TailIdx].CenterNorm) { TailIdx = Idx; } }
            FL1TimedCandidate& Tail = Events[TailIdx];
            if (bCountStylePhrase)
            {
                if (Tail.WordIndex != LastWordIndex) { continue; }
            }
            else
            {
                if (!TailWordIndices.Contains(Tail.WordIndex)) { continue; }
                if (Tail.WordIndex != LastWordIndex)
                {
                    bool bLaterVowelExists = false;
                    for (const FL1TimedCandidate& E : Events)
                    {
                        if (E.PhraseIndex == LastPhrase && E.WordIndex > Tail.WordIndex && IsVowelPose(E.PoseID) && !E.bIsLandmark && E.CenterNorm > Tail.CenterNorm)
                        {
                            bLaterVowelExists = true;
                            break;
                        }
                    }
                    if (!bLaterVowelExists) { continue; }
                }
                if (Landmarks.Num() == 0) { continue; }
                if (WordOrder.Num() == 2 && PhraseCenters.Num() <= 4 && PhraseAvgCenter < 0.84f) { continue; }
                TArray<int32>* PrevVals = IndicesByWord.Find(Tail.WordIndex - 1);
                bool bPrevLandmarks = false;
                if (PrevVals) { for (int32 Idx : *PrevVals) { if (Events[Idx].bIsLandmark) { bPrevLandmarks = true; break; } } }
                if (Landmarks.Num() >= 2 && bPrevLandmarks && PhraseAvgCenter < 0.78f && Profile.DensePhraseCount <= 1) { continue; }
                if (!bTailRiskHigh && PhraseCenters.Num() < 5) { continue; }
            }
            if (Tail.CenterNorm < 0.82f) { continue; }
            const float MaxShift = FMath::Min(0.032f, FMath::Max(0.0f, Tail.CenterNorm - 0.80f));
            const float SpanLimit = FMath::Max(0.0f, (Tail.WordSpanEndNorm - Tail.WordSpanStartNorm) * 0.18f);
            const float Shift = FMath::Min(MaxShift, SpanLimit);
            if (Shift <= 0.0f) { continue; }
            Tail.StartNorm = FMath::Max(0.0f, Tail.StartNorm - Shift);
            Tail.EndNorm = FMath::Max(Tail.StartNorm + 0.010f, Tail.EndNorm - Shift);
            Tail.CenterNorm = (Tail.StartNorm + Tail.EndNorm) * 0.5f;
            ++AdjustmentCount;
        }
    }

    static TArray<FL1TimedCandidate> ApplyLayer1CPerceptualCompression(const TArray<FL1TimedCandidate>& Raw, FOffgridAILipsyncLayer1Diagnostics& Diagnostics)
    {
        TArray<FL1TimedCandidate> Normalized;
        for (const FL1TimedCandidate& E : Raw)
        {
            FL1TimedCandidate Out = E;
            float Width = E.EndNorm - E.StartNorm;
            if (IsLandmarkPose(Out.PoseID))
            {
                Width = FMath::Clamp(Width, 0.026f, 0.058f);
                Out.Strength = FMath::Min(1.0f, Out.Strength * 1.12f);
            }
            else if (IsVowelPose(Out.PoseID))
            {
                Width = FMath::Clamp(Width, 0.045f, 0.135f);
                Out.Strength = FMath::Min(1.0f, Out.Strength * 1.08f);
            }
            Out.StartNorm = FMath::Max(0.0f, Out.CenterNorm - Width * 0.5f);
            Out.EndNorm = FMath::Min(1.0f, Out.CenterNorm + Width * 0.5f);
            Out.CenterNorm = (Out.StartNorm + Out.EndNorm) * 0.5f;
            Normalized.Add(Out);
        }
        const FAdaptiveProfile Profile = BuildAdaptiveProfile(Normalized);
        TMap<FString,int32> Reasons;
        TMap<int32,TArray<FL1TimedCandidate>> ByWord;
        for (const FL1TimedCandidate& E : Normalized) { ByWord.FindOrAdd(E.WordIndex).Add(E); }
        TArray<int32> WordKeys;
        ByWord.GetKeys(WordKeys);
        WordKeys.Sort();
        TArray<FL1TimedCandidate> Kept;
        for (int32 Wi : WordKeys)
        {
            TArray<FL1TimedCandidate> Events = ByWord[Wi];
            Events.Sort([](const FL1TimedCandidate& A, const FL1TimedCandidate& B){ return A.StartNorm == B.StartNorm ? A.Strength > B.Strength : A.StartNorm < B.StartNorm; });
            if (Events.Num() == 0) { continue; }
            const FString Word = Events[0].SourceWord;
            TMap<FName, FL1TimedCandidate> Deduped;
            for (const FL1TimedCandidate& E : Events)
            {
                FL1TimedCandidate* Existing = Deduped.Find(E.PoseID);
                if (!Existing || E.Strength > Existing->Strength)
                {
                    if (Existing) { IncReason(Reasons, TEXT("same_word_duplicate_pose")); }
                    Deduped.Add(E.PoseID, E);
                }
                else
                {
                    IncReason(Reasons, TEXT("same_word_duplicate_pose"));
                }
            }
            Deduped.GenerateValueArray(Events);
            Events.Sort([](const FL1TimedCandidate& A, const FL1TimedCandidate& B){ return A.StartNorm == B.StartNorm ? A.Strength > B.Strength : A.StartNorm < B.StartNorm; });
            TArray<FL1TimedCandidate> Vowels;
            TArray<FL1TimedCandidate> Landmarks;
            for (const FL1TimedCandidate& E : Events)
            {
                if (IsVowelPose(E.PoseID)) { Vowels.Add(E); }
                if (IsLandmarkPose(E.PoseID)) { Landmarks.Add(E); }
            }
            if (IsTinyFunctionWord(Word))
            {
                for (const FL1TimedCandidate& E : Landmarks)
                {
                    if (E.Strength >= .70f) { Kept.Add(E); }
                    else { IncReason(Reasons, TEXT("weak_function_landmark")); }
                }
                if (Vowels.Num() > 0)
                {
                    int32 Best = 0;
                    for (int32 I = 1; I < Vowels.Num(); ++I) { if (Vowels[I].Strength > Vowels[Best].Strength) { Best = I; } }
                    FString Reason;
                    if (ShouldKeepFunctionVowel(Word, Vowels[Best], Reason)) { Kept.Add(Vowels[Best]); }
                    else { IncReason(Reasons, Reason); }
                    for (int32 I = 0; I < Vowels.Num(); ++I) { if (I != Best) { IncReason(Reasons, TEXT("secondary_function_vowel")); } }
                }
                continue;
            }
            if (Word == TEXT("what") || Word == TEXT("would") || Word == TEXT("wont") || Word == TEXT("won't"))
            {
                for (const FL1TimedCandidate& E : Events)
                {
                    if (IsLandmarkPose(E.PoseID) || E.PoseID == TEXT("12_Ww-Oo-") || E.PoseID == TEXT("16_Ww-Ew-")) { Kept.Add(E); }
                    else { IncReason(Reasons, TEXT("rounded_word_secondary_vowel")); }
                }
                continue;
            }
            if (Vowels.Num() > 1 && EstimateSyllableCount(Word) <= 2)
            {
                TArray<FL1TimedCandidate> Wuh;
                for (const FL1TimedCandidate& E : Vowels) { if (E.PoseID == TEXT("12_Ww-Oo-") || E.PoseID == TEXT("16_Ww-Ew-")) { Wuh.Add(E); } }
                int32 Dominant = 0;
                for (int32 I = 1; I < Vowels.Num(); ++I)
                {
                    const float AS = Vowels[I].Strength * ((Vowels[I].PoseID == TEXT("09_Oh") || Vowels[I].PoseID == TEXT("11_Oo")) ? 1.15f : 1.0f);
                    const float BS = Vowels[Dominant].Strength * ((Vowels[Dominant].PoseID == TEXT("09_Oh") || Vowels[Dominant].PoseID == TEXT("11_Oo")) ? 1.15f : 1.0f);
                    if (AS > BS) { Dominant = I; }
                }
                for (const FL1TimedCandidate& E : Events) { if (!IsVowelPose(E.PoseID)) { Kept.Add(E); } }
                bool bDominantIsWuh = Vowels[Dominant].PoseID == TEXT("12_Ww-Oo-") || Vowels[Dominant].PoseID == TEXT("16_Ww-Ew-");
                if (Wuh.Num() > 0 && !bDominantIsWuh)
                {
                    int32 BestWuh = 0;
                    for (int32 I = 1; I < Wuh.Num(); ++I) { if (Wuh[I].Strength > Wuh[BestWuh].Strength) { BestWuh = I; } }
                    Kept.Add(Wuh[BestWuh]);
                    for (int32 I = 0; I < Wuh.Num(); ++I) { if (I != BestWuh) { IncReason(Reasons, TEXT("short_word_secondary_vowel")); } }
                }
                Kept.Add(Vowels[Dominant]);
                for (int32 I = 0; I < Vowels.Num(); ++I)
                {
                    if (I != Dominant && !(Vowels[I].PoseID == TEXT("12_Ww-Oo-") || Vowels[I].PoseID == TEXT("16_Ww-Ew-"))) { IncReason(Reasons, TEXT("short_word_secondary_vowel")); }
                }
                continue;
            }
            bool bRemovedDenseSecondary = false;
            TArray<FL1TimedCandidate> Filtered;
            for (const FL1TimedCandidate& E : Events)
            {
                const bool bDenseSecondary = E.PoseID == TEXT("14_ChJjSh") && E.LocalDensity >= 6.0f && E.Strength < .80f;
                if (bDenseSecondary)
                {
                    bool bOtherLandmark = false;
                    bool bDominantVowel = false;
                    for (const FL1TimedCandidate& Other : Events)
                    {
                        if (Other.bIsLandmark && Other.PoseID != TEXT("14_ChJjSh")) { bOtherLandmark = true; }
                        if (Other.bIsDominant) { bDominantVowel = true; }
                    }
                    if (bOtherLandmark && bDominantVowel)
                    {
                        IncReason(Reasons, TEXT("dense_secondary_sh_ch"));
                        bRemovedDenseSecondary = true;
                        continue;
                    }
                }
                Filtered.Add(E);
            }
            Kept.Append(Filtered);
        }
        Kept.Sort([](const FL1TimedCandidate& A, const FL1TimedCandidate& B){ return A.StartNorm == B.StartNorm ? A.Strength > B.Strength : A.StartNorm < B.StartNorm; });
        TArray<FL1TimedCandidate> Out;
        for (const FL1TimedCandidate& E : Kept)
        {
            const float Dur = E.EndNorm - E.StartNorm;
            if (IsVowelPose(E.PoseID) && !IsLandmarkPose(E.PoseID) && E.Strength < .60f && Dur < .070f)
            {
                IncReason(Reasons, TEXT("weak_short_vowel"));
                continue;
            }
            if (Out.Num() > 0)
            {
                FL1TimedCandidate& Prev = Out.Last();
                const bool bClose = FMath::Abs(E.CenterNorm - Prev.CenterNorm) < .026f;
                if (bClose && E.PoseID == Prev.PoseID)
                {
                    if (E.Strength >= Prev.Strength) { Prev = E; }
                    IncReason(Reasons, TEXT("close_duplicate_pose"));
                    continue;
                }
            }
            Out.Add(E);
        }
        int32 AdjustmentCount = 0;
        CompactPhraseFinalTail(Out, Profile, AdjustmentCount);
        RecomputeLocalDensity(Out);
        Diagnostics.StageCounts.CandidateCount = Raw.Num();
        Diagnostics.StageCounts.TimedCandidateCount = Raw.Num();
        Diagnostics.StageCounts.FinalEventCount = Out.Num();
        Diagnostics.SuppressionCount = Raw.Num() - Out.Num();
        Diagnostics.PhraseFinalAdjustmentCount = AdjustmentCount;
        Diagnostics.CompressionRatio = static_cast<float>(Out.Num()) / static_cast<float>(FMath::Max(1, Raw.Num()));
        return Out;
    }
}


    // ---------------------------------------------------------------------
    // Simple Viseme Budget v1 runtime port.
    //
    // Contract:
    //   Text decides what visemes exist.
    //   The budgeter decides how fully each viseme realizes.
    //   Audio anchoring and trajectory performance happen in LineCoach.
    //
    // The old Layer1A/B/C pipeline is intentionally bypassed by BuildPlan()
    // below. Keep the legacy helpers in this file for parity/rollback only;
    // do not add new runtime behavior to those paths.

    static bool SB_IsLandmarkPose(FName PoseID)
    {
        const FString Pose = PoseID.ToString();
        return Pose == TEXT("22_MBP") || Pose == TEXT("20_FV") || Pose == TEXT("21_FV-Ee-") || Pose == TEXT("19_FV-Or-")
            || Pose == TEXT("24_Tongue_Th") || Pose == TEXT("14_ChJjSh") || Pose == TEXT("12_Ww-Oo-") || Pose == TEXT("16_Ww-Ew-");
    }

    static float SB_AnchorImportance(FName PoseID)
    {
        const FString Pose = PoseID.ToString();
        if (Pose == TEXT("22_MBP")) return 1.00f;
        if (Pose == TEXT("12_Ww-Oo-")) return 0.94f;
        if (Pose == TEXT("11_Oo")) return 0.90f;
        if (Pose == TEXT("20_FV") || Pose == TEXT("21_FV-Ee-") || Pose == TEXT("19_FV-Or-")) return 0.88f;
        if (Pose == TEXT("24_Tongue_Th")) return 0.84f;
        if (Pose == TEXT("14_ChJjSh")) return 0.82f;
        if (Pose == TEXT("07_Aa")) return 0.78f;
        if (Pose == TEXT("08_Ah")) return 0.76f;
        if (Pose == TEXT("09_Oh")) return 0.74f;
        if (Pose == TEXT("10_Or")) return 0.72f;
        if (Pose == TEXT("03_Ee")) return 0.64f;
        if (Pose == TEXT("06_Eh")) return 0.60f;
        if (Pose == TEXT("04_Ih")) return 0.56f;
        if (Pose == TEXT("18_Uh")) return 0.54f;
        return 0.55f;
    }

    static bool SB_IsFunctionOrReducedWord(const FString& Word)
    {
        return IsReducedWord(Word);
    }

    static float SB_WordUnit(const FString& Word)
    {
        // Speech-only word budget. This is not a pause model and not a
        // whole-line duration model. It only distributes articulation time
        // inside the trusted text viseme plan.
        const bool bReduced = SB_IsFunctionOrReducedWord(Word);
        const int32 Syllables = EstimateSyllableCount(Word);

        float Unit = bReduced ? 0.50f : 0.90f;

        // Extra syllables add articulation targets, but conversational speech
        // does not give each syllable a full word's worth of visible time.
        if (Syllables > 1)
        {
            Unit += (bReduced ? 0.10f : 0.18f) * static_cast<float>(Syllables - 1);
        }

        // Interjections are usually deliberately shaped and readable.
        if (Word == TEXT("oh") || Word == TEXT("wow") || Word == TEXT("well") || Word == TEXT("hey") || Word == TEXT("okay"))
        {
            Unit += 0.22f;
        }

        return FMath::Clamp(Unit, 0.22f, 1.55f);
    }

    struct FSBTimingShape
    {
        int32 PhraseCount = 1;
        int32 MaxPhraseWords = 0;
        int32 MinPhraseWords = 0;
        int32 ShortPhraseCount = 0;
        int32 FirstPhraseWords = 0;
        int32 LastPhraseWords = 0;
        int32 ProperNounLikeCount = 0;
        int32 LongWordCount = 0;
        int32 UnusualWordCount = 0;
        float ReducedRatio = 0.0f;
        float MeanWordLen = 0.0f;
        float MeanSyllables = 0.0f;
        bool bCadenceList = false;
        bool bLongNarrative = false;
        bool bLongDeclarative = false;
        bool bProperNounHeavy = false;
        bool bSentenceChain = false;
        bool bCompactConversationalTurn = false;
        bool bMultiClauseExplainer = false;
    };

    static FSBTimingShape SB_AnalyzeTimingShape(const TArray<FWordSpan>& Words, const TArray<FWordSpan>* OriginalWords = nullptr)
    {
        FSBTimingShape Shape;
        if (Words.Num() <= 0)
        {
            return Shape;
        }

        int32 CadenceWords = 0;
        int32 OrdinalWords = 0;
        int32 ReducedWords = 0;
        int32 MeaningfulWords = 0;
        int32 ShortSyllableWords = 0;
        int32 PhraseWords = 0;
        int32 LastPhrase = Words[0].PhraseIndex;
        float WordLenSum = 0.0f;
        float SyllableSum = 0.0f;

        Shape.PhraseCount = 1;
        Shape.MinPhraseWords = TNumericLimits<int32>::Max();
        int32 PhraseOrdinal = 0;
        for (const FWordSpan& W : Words)
        {
            if (W.PhraseIndex != LastPhrase)
            {
                if (PhraseOrdinal == 0)
                {
                    Shape.FirstPhraseWords = PhraseWords;
                }
                Shape.MaxPhraseWords = FMath::Max(Shape.MaxPhraseWords, PhraseWords);
                Shape.MinPhraseWords = FMath::Min(Shape.MinPhraseWords, PhraseWords);
                Shape.ShortPhraseCount += PhraseWords <= 4 ? 1 : 0;
                PhraseWords = 0;
                LastPhrase = W.PhraseIndex;
                ++Shape.PhraseCount;
                ++PhraseOrdinal;
            }

            const int32 Syllables = EstimateSyllableCount(W.Word);
            const bool bReduced = IsReducedWord(W.Word);
            const bool bMeaningful = IsMeaningfulWord(W.Word);
            const bool bLongWord = W.Word.Len() >= 8;
            const bool bUnusualWord = W.Word.Len() >= 9 || Syllables >= 4;
            bool bProperNounLike = false;
            if (OriginalWords != nullptr && OriginalWords->IsValidIndex(&W - Words.GetData()))
            {
                const FString& OriginalWord = (*OriginalWords)[&W - Words.GetData()].Word;
                bProperNounLike = IsTitleCaseWord(OriginalWord) && OriginalWord.Len() >= 5;
            }

            CadenceWords += IsCountCadenceWord(W.Word) ? 1 : 0;
            OrdinalWords += IsOrdinalCadenceWord(W.Word) ? 1 : 0;
            ReducedWords += bReduced ? 1 : 0;
            MeaningfulWords += bMeaningful ? 1 : 0;
            ShortSyllableWords += Syllables <= 2 ? 1 : 0;
            Shape.LongWordCount += bLongWord ? 1 : 0;
            Shape.UnusualWordCount += bUnusualWord ? 1 : 0;
            Shape.ProperNounLikeCount += bProperNounLike ? 1 : 0;
            WordLenSum += static_cast<float>(W.Word.Len());
            SyllableSum += static_cast<float>(Syllables);
            ++PhraseWords;
        }
        Shape.MaxPhraseWords = FMath::Max(Shape.MaxPhraseWords, PhraseWords);
        Shape.MinPhraseWords = FMath::Min(Shape.MinPhraseWords, PhraseWords);
        Shape.ShortPhraseCount += PhraseWords <= 4 ? 1 : 0;
        if (Shape.FirstPhraseWords == 0)
        {
            Shape.FirstPhraseWords = PhraseWords;
        }
        Shape.LastPhraseWords = PhraseWords;
        Shape.ReducedRatio = static_cast<float>(ReducedWords) / static_cast<float>(FMath::Max(1, Words.Num()));
        Shape.MeanWordLen = WordLenSum / static_cast<float>(FMath::Max(1, Words.Num()));
        Shape.MeanSyllables = SyllableSum / static_cast<float>(FMath::Max(1, Words.Num()));

        const bool bExplicitCadence = CadenceWords >= FMath::Max(2, Words.Num() - 1)
            || CadenceWords + OrdinalWords >= FMath::Max(3, Words.Num() - 1);
        const bool bCommaLikeList = Shape.PhraseCount >= 3 && Shape.MaxPhraseWords <= 2 && Shape.ReducedRatio <= 0.26f;
        const bool bShortContentChain = Shape.PhraseCount == 1
            && Words.Num() >= 5
            && MeaningfulWords >= Words.Num() - 1
            && Shape.ReducedRatio <= 0.14f
            && Shape.MeanWordLen <= 6.6f
            && Shape.MeanSyllables <= 2.35f
            && ShortSyllableWords >= Words.Num() - 1;

        Shape.bCadenceList = bExplicitCadence || bCommaLikeList || bShortContentChain;
        Shape.bLongNarrative = !Shape.bCadenceList
            && Words.Num() >= 10
            && Shape.ReducedRatio <= 0.30f
            && (Shape.MeanWordLen >= 4.7f || Shape.MeanSyllables >= 1.75f);
        Shape.bLongDeclarative = !Shape.bCadenceList
            && Shape.PhraseCount == 1
            && Words.Num() >= 8
            && Shape.ReducedRatio <= 0.34f;
        Shape.bProperNounHeavy = !Shape.bCadenceList
            && (Shape.ProperNounLikeCount >= 2
                || (Shape.ProperNounLikeCount >= 1 && Shape.LongWordCount >= 3)
                || (Shape.ProperNounLikeCount >= 1 && Shape.UnusualWordCount >= 2));
        Shape.bSentenceChain = !Shape.bCadenceList
            && Words.Num() <= 12
            && Shape.PhraseCount >= 3
            && Shape.ShortPhraseCount >= Shape.PhraseCount - 1
            && Shape.MaxPhraseWords <= 5;
        Shape.bCompactConversationalTurn = !Shape.bCadenceList
            && Words.Num() <= 11
            && Shape.PhraseCount >= 2
            && Shape.MaxPhraseWords <= 6
            && Shape.MeanWordLen <= 5.8f
            && Shape.MeanSyllables <= 2.0f
            && Shape.ReducedRatio >= 0.12f;
        Shape.bMultiClauseExplainer = !Shape.bCadenceList
            && !Shape.bSentenceChain
            && Words.Num() >= 10
            && Shape.PhraseCount >= 2
            && Shape.MaxPhraseWords >= 6
            && Shape.ReducedRatio <= 0.36f
            && (Shape.FirstPhraseWords - Shape.LastPhraseWords >= 3
                || Shape.LastPhraseWords <= 7
                || Shape.MaxPhraseWords - Shape.MinPhraseWords >= 4);
        return Shape;
    }

    static float SB_EstimateDurationSeconds(const FString& Text, const TArray<FWordSpan>& Words, const TArray<FWordSpan>* OriginalWords, float MinDurationSeconds)
    {
        (void)OriginalWords;

        // Clear contract: estimate speech articulation time only.
        //
        // CMUdict gives us phoneme identity and stress; it does not give pause
        // timing. End-of-sentence pauses are handled by text islands and audio
        // island starts. Commas are treated as tiny, text-predictable internal
        // articulation gaps, not as phrase-end stretch.
        if (Words.Num() <= 0)
        {
            return FMath::Max(0.10f, MinDurationSeconds);
        }

        float Duration = 0.12f; // small attack/settle allowance, not a pause.
        int32 CadenceWords = 0;
        int32 OrdinalWords = 0;
        int32 ReducedWords = 0;
        int32 ContentWords = 0;

        for (const FWordSpan& W : Words)
        {
            const bool bReduced = IsReducedWord(W.Word);
            const bool bContent = IsMeaningfulWord(W.Word);
            const bool bCadenceWord = IsCountCadenceWord(W.Word);
            const bool bOrdinalWord = IsOrdinalCadenceWord(W.Word);
            const int32 Syllables = EstimateSyllableCount(W.Word);

            float Unit = SB_WordUnit(W.Word);

            // Cadence/list readings are intentionally a little more deliberate.
            if (bCadenceWord) { Unit += 0.20f; }
            if (bOrdinalWord) { Unit += 0.12f; }

            // Long content words need some extra room, but do not let spelling
            // length dominate over syllable/articulation count.
            if (bContent && Syllables >= 3) { Unit += 0.08f; }

            // This is the speech-rate prior for visible articulation.  The previous
            // cleanup made this 0.115s/unit, which was clean but far too short:
            // full 30-event lines could finish their visible mouth motion halfway
            // through the audio.  Keep the model simple, but give the monotonic
            // queue enough wall-clock time to remain alive for the spoken line.
            Duration += 0.175f * Unit;

            CadenceWords += bCadenceWord ? 1 : 0;
            OrdinalWords += bOrdinalWord ? 1 : 0;
            ReducedWords += bReduced ? 1 : 0;
            ContentWords += bContent ? 1 : 0;
        }

        int32 Commas = 0;
        for (TCHAR C : Text)
        {
            if (C == TEXT(','))
            {
                ++Commas;
            }
        }
        Duration += FMath::Min(0.12f, 0.035f * static_cast<float>(Commas));

        const float ReducedRatio = static_cast<float>(ReducedWords) / static_cast<float>(FMath::Max(1, Words.Num()));
        const bool bCadenceList = CadenceWords >= FMath::Max(2, Words.Num() - 1)
            || CadenceWords + OrdinalWords >= FMath::Max(3, Words.Num() - 1);

        if (bCadenceList)
        {
            Duration *= 1.08f;
        }
        else if (ReducedRatio >= 0.40f && ContentWords <= Words.Num() / 2)
        {
            Duration *= 0.92f;
        }

        // This estimate is deliberately allowed to be short. Audio island starts
        // are authoritative for pauses and phrase launches; the text planner's
        // job is to avoid making the monotonic viseme queue inherently late.
        return FMath::Clamp(FMath::Max(MinDurationSeconds, Duration), 0.35f, 8.0f);
    }

    struct FSBSpec
    {
        FName PoseID = NAME_None;
        float Fraction = 0.0f;
        float WidthFraction = 0.0f;
        bool bHasLandmarkOverride = false;
        bool bLandmarkOverride = false;
        bool bHasImportanceOverride = false;
        float ImportanceOverride = 0.0f;
        FName SupportKind = NAME_None;
    };

    static void SB_AddSpec(TArray<FSBSpec>& Specs, FName PoseID, float Fraction, float WidthFraction)
    {
        FSBSpec S;
        S.PoseID = PoseID;
        S.Fraction = FMath::Clamp(Fraction, 0.04f, 0.96f);
        S.WidthFraction = FMath::Max(0.05f, WidthFraction);
        Specs.Add(S);
    }

    static void SB_AddSupportSpec(TArray<FSBSpec>& Specs, FName PoseID, float Fraction, float WidthFraction, float ImportanceOverride, FName SupportKind)
    {
        FSBSpec S;
        S.PoseID = PoseID;
        S.Fraction = FMath::Clamp(Fraction, 0.04f, 0.96f);
        S.WidthFraction = FMath::Max(0.05f, WidthFraction);
        S.bHasLandmarkOverride = true;
        S.bLandmarkOverride = false;
        S.bHasImportanceOverride = true;
        S.ImportanceOverride = ImportanceOverride;
        S.SupportKind = SupportKind;
        Specs.Add(S);
    }

    static void SB_AddDominantVowelSpec(TArray<FSBSpec>& Specs, FName PoseID, float Fraction, float WidthFraction)
    {
        FSBSpec S;
        S.PoseID = PoseID;
        S.Fraction = FMath::Clamp(Fraction, 0.04f, 0.96f);
        S.WidthFraction = FMath::Max(0.05f, WidthFraction);
        S.bHasLandmarkOverride = true;
        S.bLandmarkOverride = false;
        S.bHasImportanceOverride = true;
        S.ImportanceOverride = FMath::Max(0.82f, SB_AnchorImportance(PoseID));
        S.SupportKind = FName(TEXT("stressed_vowel"));
        Specs.Add(S);
    }

    static void SB_CleanupSpecs(TArray<FSBSpec>& Specs);

    static std::string SB_AsciiLower(const FString& Text)
    {
        std::string Out;
        Out.reserve(static_cast<size_t>(Text.Len()));
        for (int32 I = 0; I < Text.Len(); ++I)
        {
            const unsigned char C = static_cast<unsigned char>(Text[I]);
            Out.push_back(static_cast<char>(std::tolower(C)));
        }
        return Out;
    }

    static FString SB_NormalizeTemplateWord(const FString& Word)
    {
        FString Key = Word.ToLower();
        if (Key.EndsWith(TEXT("'s"))) { Key.LeftChopInline(2); }
        else if (Key.EndsWith(TEXT("s'"))) { Key.LeftChopInline(1); }
        Key.ReplaceInline(TEXT("-"), TEXT(""));
        return Key;
    }

    static bool SB_EndsWithAny(const FString& Word, std::initializer_list<const TCHAR*> Suffixes)
    {
        for (const TCHAR* Suffix : Suffixes)
        {
            if (Word.EndsWith(Suffix))
            {
                return true;
            }
        }
        return false;
    }

    static bool SB_ContainsAny(const FString& Word, std::initializer_list<const TCHAR*> Parts)
    {
        for (const TCHAR* Part : Parts)
        {
            if (Word.Contains(Part))
            {
                return true;
            }
        }
        return false;
    }

    static bool SB_StartsWithAny(const FString& Word, std::initializer_list<const TCHAR*> Prefixes)
    {
        for (const TCHAR* Prefix : Prefixes)
        {
            if (Word.StartsWith(Prefix))
            {
                return true;
            }
        }
        return false;
    }

    static bool SB_IsVowelLikePose(FName PoseID)
    {
        const FString Pose = PoseID.ToString();
        return Pose == TEXT("03_Ee") || Pose == TEXT("04_Ih") || Pose == TEXT("05_Ay") ||
            Pose == TEXT("06_Eh") || Pose == TEXT("07_Aa") || Pose == TEXT("08_Ah") ||
            Pose == TEXT("09_Oh") || Pose == TEXT("10_Or") || Pose == TEXT("11_Oo") ||
            Pose == TEXT("18_Uh");
    }

    static float SB_SpecImportanceForCleanup(const FSBSpec& Spec)
    {
        return Spec.bHasImportanceOverride ? Spec.ImportanceOverride : SB_AnchorImportance(Spec.PoseID);
    }

    static bool SB_HasSpecNear(const TArray<FSBSpec>& Specs, FName PoseID, float Fraction, float Tolerance)
    {
        for (const FSBSpec& Spec : Specs)
        {
            if (Spec.PoseID == PoseID && FMath::Abs(Spec.Fraction - Fraction) <= Tolerance)
            {
                return true;
            }
        }
        return false;
    }

    static bool SB_HasAnySpecNear(const TArray<FSBSpec>& Specs, std::initializer_list<const TCHAR*> Poses, float Fraction, float Tolerance)
    {
        for (const TCHAR* Pose : Poses)
        {
            if (SB_HasSpecNear(Specs, FName(Pose), Fraction, Tolerance))
            {
                return true;
            }
        }
        return false;
    }

    static bool SB_NeedsPlainSibilantOnset(const FString& Word)
    {
        if (Word.IsEmpty()) { return false; }
        if (Word.StartsWith(TEXT("sh")) || Word.StartsWith(TEXT("ch")) || Word.StartsWith(TEXT("j"))) { return false; }
        return Word.StartsWith(TEXT("s")) || Word.StartsWith(TEXT("z")) || Word.StartsWith(TEXT("x"));
    }

    static int32 SB_FindVisibleRoundGlideIndex(const FString& Word)
    {
        const FString Key = SB_NormalizeTemplateWord(Word);
        if (Key.Len() < 2) { return INDEX_NONE; }
        if (Key == TEXT("two")) { return INDEX_NONE; }
        if (Key.StartsWith(TEXT("wr")) || Key.StartsWith(TEXT("who")) || Key.StartsWith(TEXT("whol"))) { return INDEX_NONE; }
        if (Key.Contains(TEXT("answer")) || Key.Contains(TEXT("sword"))) { return INDEX_NONE; }
        if (Key.StartsWith(TEXT("sw"))) { return 1; }
        if (Key.StartsWith(TEXT("qu"))) { return 1; }

        for (int32 I = 1; I + 1 < Key.Len(); ++I)
        {
            if (Key[I] != TEXT('w')) { continue; }
            const TCHAR Next = Key[I + 1];
            if (IsVowelChar(Next) || Next == TEXT('y'))
            {
                return I;
            }
        }
        return INDEX_NONE;
    }

    static bool SB_IsCompactContraction(const FString& Word)
    {
        const FString Key = SB_NormalizeTemplateWord(Word);
        return Key.Contains(TEXT("'")) ||
            Key == TEXT("weve") || Key == TEXT("whatll") || Key == TEXT("youre") || Key == TEXT("im") ||
            Key == TEXT("dont") || Key == TEXT("cant");
    }

    static std::string SB_CmuBaseWord(const std::string& Word)
    {
        const size_t Paren = Word.find('(');
        if (Paren != std::string::npos)
        {
            return Word.substr(0, Paren);
        }
        return Word;
    }

    static void SB_LoadCmuLexiconFromStream(std::istream& In, std::unordered_map<std::string, std::vector<std::string>>& Lexicon)
    {
        std::string Line;
        while (std::getline(In, Line))
        {
            if (Line.empty() || Line[0] == '#')
            {
                continue;
            }

            std::istringstream Stream(Line);
            std::string Word;
            Stream >> Word;
            if (Word.empty())
            {
                continue;
            }

            const std::string Base = SB_CmuBaseWord(Word);
            if (Lexicon.find(Base) != Lexicon.end())
            {
                continue;
            }

            std::vector<std::string> Phones;
            std::string Phone;
            while (Stream >> Phone)
            {
                Phones.push_back(Phone);
            }
            if (!Phones.empty())
            {
                Lexicon.emplace(Base, std::move(Phones));
            }
        }
    }

    static std::unordered_map<std::string, std::vector<std::string>>& SB_CmuLexicon()
    {
        static std::unordered_map<std::string, std::vector<std::string>> Lexicon;
        static bool bLoaded = false;
        if (bLoaded)
        {
            return Lexicon;
        }
        bLoaded = true;

        const char* EmbeddedData = OffgridAILipsyncEmbedded::GetCmudictDictData();
        const int32 EmbeddedSize = OffgridAILipsyncEmbedded::GetCmudictDictDataSize();
        if (EmbeddedData && EmbeddedSize > 0)
        {
            std::istringstream EmbeddedStream(std::string(EmbeddedData, static_cast<size_t>(EmbeddedSize)));
            SB_LoadCmuLexiconFromStream(EmbeddedStream, Lexicon);
        }

        if (Lexicon.empty())
        {
            std::ifstream In("third_party/cmudict/cmudict.dict");
            if (!In.is_open())
            {
                In.open("../third_party/cmudict/cmudict.dict");
            }
            if (In.is_open())
            {
                SB_LoadCmuLexiconFromStream(In, Lexicon);
            }
        }

        return Lexicon;
    }

    static const std::unordered_map<std::string, std::vector<std::string>>& SB_PronunciationOverrides()
    {
        static const std::unordered_map<std::string, std::vector<std::string>> Overrides = {
            { "weve", { "W", "IY1", "V" } },
            { "we've", { "W", "IY1", "V" } },
            { "we'll", { "W", "IY1", "L" } },
            { "well", { "W", "EH1", "L" } },
            { "whatll", { "W", "AH1", "T", "AH0", "L" } },
            { "what'll", { "W", "AH1", "T", "AH0", "L" } }
        };
        return Overrides;
    }

    static std::string SB_CmuLookupKey(const FString& Word)
    {
        FString Key = Word.ToLower();
        Key.ReplaceInline(TEXT("-"), TEXT(""));
        return SB_AsciiLower(Key);
    }

    static bool SB_FindPronunciationForWord(const FString& Word, std::vector<std::string>& OutPhones)
    {
        const std::string Key = SB_CmuLookupKey(Word);
        const auto& Overrides = SB_PronunciationOverrides();
        if (const auto It = Overrides.find(Key); It != Overrides.end())
        {
            OutPhones = It->second;
            return true;
        }

        auto& Lexicon = SB_CmuLexicon();
        if (const auto It = Lexicon.find(Key); It != Lexicon.end())
        {
            OutPhones = It->second;
            return true;
        }

        std::string PossessiveBase = Key;
        if (PossessiveBase.size() > 2 && PossessiveBase.substr(PossessiveBase.size() - 2) == "'s")
        {
            PossessiveBase.resize(PossessiveBase.size() - 2);
        }
        else if (PossessiveBase.size() > 1 && PossessiveBase.back() == 's')
        {
            PossessiveBase.pop_back();
        }
        if (PossessiveBase != Key)
        {
            if (const auto It = Lexicon.find(PossessiveBase); It != Lexicon.end())
            {
                OutPhones = It->second;
                OutPhones.push_back("Z");
                return true;
            }
        }
        return false;
    }

    static std::string SB_BasePhone(const std::string& Phone)
    {
        std::string Base;
        Base.reserve(Phone.size());
        for (const char C : Phone)
        {
            if (!std::isdigit(static_cast<unsigned char>(C)))
            {
                Base.push_back(C);
            }
        }
        return Base;
    }

    static int32 SB_PhoneStress(const std::string& Phone)
    {
        for (const char C : Phone)
        {
            if (C == '1' || C == '2' || C == '0')
            {
                return C - '0';
            }
        }
        return INDEX_NONE;
    }

    static bool SB_IsVowelPhone(const std::string& Base)
    {
        return Base == "AA" || Base == "AE" || Base == "AH" || Base == "AO" ||
            Base == "AW" || Base == "AY" || Base == "EH" || Base == "ER" ||
            Base == "EY" || Base == "IH" || Base == "IY" || Base == "OW" ||
            Base == "OY" || Base == "UH" || Base == "UW";
    }

    static FName SB_PoseForVowelPhone(const std::string& Base, int32 Stress)
    {
        if (Base == "IY") { return TEXT("03_Ee"); }
        if (Base == "IH") { return TEXT("04_Ih"); }
        if (Base == "EH") { return TEXT("06_Eh"); }
        if (Base == "AE" || Base == "AA") { return TEXT("07_Aa"); }
        if (Base == "AH") { return Stress == 0 ? FName(TEXT("18_Uh")) : FName(TEXT("07_Aa")); }
        if (Base == "AO" || Base == "OW") { return TEXT("09_Oh"); }
        if (Base == "UH") { return Stress == 0 ? FName(TEXT("18_Uh")) : FName(TEXT("11_Oo")); }
        if (Base == "UW") { return TEXT("11_Oo"); }
        if (Base == "ER") { return TEXT("10_Or"); }
        if (Base == "AY" || Base == "EY") { return TEXT("05_Ay"); }
        if (Base == "AW") { return TEXT("11_Oo"); }
        if (Base == "OY") { return TEXT("09_Oh"); }
        return NAME_None;
    }

    static FName SB_PoseForConsonantPhone(const std::string& Base)
    {
        if (Base == "P" || Base == "B" || Base == "M") { return TEXT("22_MBP"); }
        if (Base == "F" || Base == "V") { return TEXT("20_FV"); }
        if (Base == "W") { return TEXT("12_Ww-Oo-"); }
        if (Base == "Y") { return TEXT("16_Ww-Ew-"); }
        if (Base == "SH" || Base == "CH" || Base == "JH" || Base == "ZH" || Base == "S" || Base == "Z") { return TEXT("14_ChJjSh"); }
        if (Base == "TH" || Base == "DH") { return TEXT("24_Tongue_Th"); }
        return NAME_None;
    }

    static int32 SB_PrimaryVowelPhoneIndex(const std::vector<std::string>& Phones)
    {
        int32 Secondary = INDEX_NONE;
        int32 FirstVowel = INDEX_NONE;
        for (int32 I = 0; I < static_cast<int32>(Phones.size()); ++I)
        {
            const std::string Base = SB_BasePhone(Phones[static_cast<size_t>(I)]);
            if (!SB_IsVowelPhone(Base))
            {
                continue;
            }
            if (FirstVowel == INDEX_NONE)
            {
                FirstVowel = I;
            }
            const int32 Stress = SB_PhoneStress(Phones[static_cast<size_t>(I)]);
            if (Stress == 1)
            {
                return I;
            }
            if (Stress == 2 && Secondary == INDEX_NONE)
            {
                Secondary = I;
            }
        }
        return Secondary != INDEX_NONE ? Secondary : FirstVowel;
    }

    static void SB_AddPronunciationVowelSpec(TArray<FSBSpec>& Specs, FName Pose, float Fraction, bool bDominant, int32 Stress, bool bTerminal)
    {
        if (Pose == NAME_None)
        {
            return;
        }
        if (bDominant || Stress == 1)
        {
            SB_AddDominantVowelSpec(Specs, Pose, Fraction, 0.36f);
            return;
        }
        const float Importance = Stress == 2 ? 0.58f : (bTerminal ? 0.42f : 0.50f);
        const TCHAR* Kind = bTerminal ? TEXT("terminal_reduction_support") : TEXT("medial_vowel_support");
        SB_AddSupportSpec(Specs, Pose, Fraction, bTerminal ? 0.20f : 0.24f, Importance, Kind);
    }

    static TArray<FSBSpec> SB_PronunciationSpecsForWord(const FString& Word, bool& bOutUsedPronunciation)
    {
        bOutUsedPronunciation = false;
        const std::string LookupKey = SB_CmuLookupKey(Word);
        if (LookupKey == "we've" || LookupKey == "weve")
        {
            bOutUsedPronunciation = true;
            TArray<FSBSpec> Compact;
            SB_AddSpec(Compact, TEXT("12_Ww-Oo-"), 0.18f, 0.14f);
            SB_AddSpec(Compact, TEXT("20_FV"), 0.62f, 0.22f);
            return Compact;
        }
        if (LookupKey == "hello")
        {
            bOutUsedPronunciation = true;
            TArray<FSBSpec> Greeting;
            SB_AddDominantVowelSpec(Greeting, TEXT("06_Eh"), 0.30f, 0.46f);
            SB_AddDominantVowelSpec(Greeting, TEXT("09_Oh"), 0.68f, 0.54f);
            return Greeting;
        }
        if (LookupKey == "what")
        {
            bOutUsedPronunciation = true;
            TArray<FSBSpec> Interrogative;
            SB_AddSpec(Interrogative, TEXT("12_Ww-Oo-"), 0.16f, 0.26f);
            SB_AddSpec(Interrogative, TEXT("07_Aa"), 0.58f, 0.40f);
            return Interrogative;
        }
        if (LookupKey == "what'll" || LookupKey == "whatll")
        {
            bOutUsedPronunciation = true;
            TArray<FSBSpec> Compact;
            SB_AddSpec(Compact, TEXT("12_Ww-Oo-"), 0.16f, 0.16f);
            SB_AddDominantVowelSpec(Compact, TEXT("07_Aa"), 0.52f, 0.32f);
            SB_AddSupportSpec(Compact, TEXT("18_Uh"), 0.78f, 0.18f, 0.36f, TEXT("terminal_reduction_support"));
            return Compact;
        }

        std::vector<std::string> Phones;
        if (!SB_FindPronunciationForWord(Word, Phones) || Phones.empty())
        {
            return TArray<FSBSpec>();
        }
        bOutUsedPronunciation = true;

        TArray<FSBSpec> Specs;
        const int32 PhoneCount = static_cast<int32>(Phones.size());
        const int32 DominantVowelIndex = SB_PrimaryVowelPhoneIndex(Phones);
        for (int32 I = 0; I < PhoneCount; ++I)
        {
            const std::string Base = SB_BasePhone(Phones[static_cast<size_t>(I)]);
            const int32 Stress = SB_PhoneStress(Phones[static_cast<size_t>(I)]);
            const float Fraction = FMath::Clamp((static_cast<float>(I) + 0.50f) / static_cast<float>(FMath::Max(1, PhoneCount)), 0.06f, 0.94f);
            const bool bTerminal = I >= PhoneCount - 2;

            if (Base == "Y" && I + 1 < PhoneCount && SB_BasePhone(Phones[static_cast<size_t>(I + 1)]) == "UW")
            {
                const int32 NextStress = SB_PhoneStress(Phones[static_cast<size_t>(I + 1)]);
                const bool bDominantYoo = (I + 1) == DominantVowelIndex || NextStress == 1;
                if (bDominantYoo)
                {
                    SB_AddSpec(Specs, TEXT("16_Ww-Ew-"), FMath::Clamp(Fraction + 0.04f, 0.06f, 0.94f), 0.26f);
                }
                else
                {
                    SB_AddSupportSpec(Specs, TEXT("16_Ww-Ew-"), Fraction, 0.18f, 0.54f, TEXT("round_syllable_support"));
                }
                continue;
            }

            if (SB_IsVowelPhone(Base))
            {
                if (Base == "AW")
                {
                    SB_AddSupportSpec(Specs, TEXT("07_Aa"), FMath::Clamp(Fraction - 0.04f, 0.06f, 0.94f), 0.14f, 0.48f, TEXT("medial_vowel_support"));
                    SB_AddPronunciationVowelSpec(Specs, TEXT("11_Oo"), FMath::Clamp(Fraction + 0.04f, 0.06f, 0.94f), I == DominantVowelIndex, Stress, bTerminal);
                    continue;
                }
                if (Base == "OY")
                {
                    SB_AddSupportSpec(Specs, TEXT("09_Oh"), FMath::Clamp(Fraction - 0.04f, 0.06f, 0.94f), 0.14f, 0.48f, TEXT("medial_vowel_support"));
                    SB_AddPronunciationVowelSpec(Specs, TEXT("05_Ay"), FMath::Clamp(Fraction + 0.04f, 0.06f, 0.94f), I == DominantVowelIndex, Stress, bTerminal);
                    continue;
                }
                SB_AddPronunciationVowelSpec(Specs, SB_PoseForVowelPhone(Base, Stress), Fraction, I == DominantVowelIndex, Stress, bTerminal);
                continue;
            }

            const FName ConsonantPose = SB_PoseForConsonantPhone(Base);
            if (ConsonantPose == NAME_None)
            {
                continue;
            }

            if (ConsonantPose == TEXT("22_MBP") || ConsonantPose == TEXT("20_FV"))
            {
                SB_AddSpec(Specs, ConsonantPose, Fraction, 0.20f);
            }
            else if (ConsonantPose == TEXT("12_Ww-Oo-"))
            {
                if (I > 0)
                {
                    SB_AddSpec(Specs, ConsonantPose, Fraction, 0.20f);
                }
                else
                {
                    SB_AddSupportSpec(Specs, ConsonantPose, Fraction, 0.16f, 0.54f, TEXT("round_glide_support"));
                }
            }
            else if (ConsonantPose == TEXT("16_Ww-Ew-"))
            {
                SB_AddSupportSpec(Specs, ConsonantPose, Fraction, 0.18f, 0.54f, TEXT("round_syllable_support"));
            }
            else if (ConsonantPose == TEXT("24_Tongue_Th"))
            {
                SB_AddSupportSpec(Specs, ConsonantPose, Fraction, 0.14f, 0.36f, TEXT("dental_support"));
            }
            else
            {
                const bool bStrongAffricate = Base == "SH" || Base == "CH" || Base == "JH" || Base == "ZH";
                if (bStrongAffricate)
                {
                    SB_AddSpec(Specs, ConsonantPose, Fraction, 0.18f);
                }
                else
                {
                    SB_AddSupportSpec(Specs, ConsonantPose, Fraction, 0.14f, bTerminal ? 0.40f : 0.46f, bTerminal ? TEXT("final_sibilant_support") : TEXT("sibilant_onset"));
                }
            }
        }

        SB_CleanupSpecs(Specs);
        return Specs;
    }

    enum class ESBVisualRole : uint8
    {
        Dominant,
        Support,
        Transient,
        Suppressed
    };

    struct FSBVisualCandidate
    {
        FName PoseID = NAME_None;
        float Fraction = 0.0f;
        float WidthFraction = 0.0f;
        float Importance = 0.0f;
        FName SupportKind = NAME_None;
        ESBVisualRole Role = ESBVisualRole::Support;
    };

    static void SB_AddRoleCandidate(
        TArray<FSBVisualCandidate>& Candidates,
        const TCHAR* Pose,
        float Fraction,
        float Width,
        float Importance,
        const TCHAR* Kind,
        ESBVisualRole Role)
    {
        FSBVisualCandidate C;
        C.PoseID = FName(Pose);
        C.Fraction = FMath::Clamp(Fraction, 0.04f, 0.96f);
        C.WidthFraction = FMath::Max(0.05f, Width);
        C.Importance = FMath::Clamp(Importance, 0.0f, 1.0f);
        C.SupportKind = FName(Kind);
        C.Role = Role;
        Candidates.Add(C);
    }

    static bool SB_HasRColoredEnding(const FString& Word)
    {
        return SB_EndsWithAny(Word, {
            TEXT("er"), TEXT("ers"), TEXT("or"), TEXT("ors"), TEXT("ar"), TEXT("ars"),
            TEXT("ir"), TEXT("ur"), TEXT("ort"), TEXT("orts"), TEXT("ert"), TEXT("erts")
        });
    }

    static bool SB_NeedsYooOrRoundedSyllable(const FString& Word)
    {
        return Word.Contains(TEXT("commu")) || Word.Contains(TEXT("muni")) || Word.Contains(TEXT("music"))
            || Word.Contains(TEXT("human")) || Word.Contains(TEXT("future")) || Word.Contains(TEXT("usual"))
            || Word.Contains(TEXT("univ")) || Word.Contains(TEXT("unit"));
    }

    static TArray<FSBVisualCandidate> SB_ExtractVisualCandidatesForWord(const FString& Word)
    {
        const FString Key = SB_NormalizeTemplateWord(Word);
        TArray<FSBVisualCandidate> Candidates;
        if (Key.IsEmpty())
        {
            return Candidates;
        }

        if (SB_NeedsPlainSibilantOnset(Key))
        {
            SB_AddRoleCandidate(Candidates, TEXT("14_ChJjSh"), 0.10f, 0.16f, 0.46f, TEXT("sibilant_onset"), ESBVisualRole::Transient);
        }

        const int32 RoundGlideIndex = SB_FindVisibleRoundGlideIndex(Key);
        if (RoundGlideIndex != INDEX_NONE)
        {
            const float Fraction = FMath::Clamp((static_cast<float>(RoundGlideIndex) + 0.50f) / static_cast<float>(FMath::Max(1, Key.Len())), 0.16f, 0.70f);
            SB_AddRoleCandidate(Candidates, TEXT("12_Ww-Oo-"), Fraction, 0.15f, 0.48f, TEXT("round_glide_support"), ESBVisualRole::Support);
        }

        if (SB_NeedsYooOrRoundedSyllable(Key))
        {
            SB_AddRoleCandidate(Candidates, TEXT("16_Ww-Ew-"), 0.48f, 0.20f, 0.62f, TEXT("round_syllable_support"), ESBVisualRole::Support);
        }

        if (SB_HasRColoredEnding(Key))
        {
            SB_AddRoleCandidate(Candidates, TEXT("10_Or"), 0.82f, 0.22f, 0.54f, TEXT("rhotic_support"), ESBVisualRole::Support);
        }

        if ((Key.EndsWith(TEXT("s")) || Key.EndsWith(TEXT("z")) || Key.EndsWith(TEXT("ce")) || Key.EndsWith(TEXT("se"))) &&
            !Key.EndsWith(TEXT("ss")))
        {
            SB_AddRoleCandidate(Candidates, TEXT("14_ChJjSh"), 0.88f, 0.14f, 0.36f, TEXT("final_sibilant_support"), ESBVisualRole::Transient);
        }

        return Candidates;
    }

    static void SB_ApplyRoleCandidates(const FString& Word, TArray<FSBSpec>& Specs)
    {
        const TArray<FSBVisualCandidate> Candidates = SB_ExtractVisualCandidatesForWord(Word);
        for (const FSBVisualCandidate& C : Candidates)
        {
            if (C.Role == ESBVisualRole::Suppressed || C.PoseID == NAME_None)
            {
                continue;
            }
            if (SB_HasSpecNear(Specs, C.PoseID, C.Fraction, 0.16f))
            {
                continue;
            }
            if (SB_HasAnySpecNear(Specs, { TEXT("12_Ww-Oo-"), TEXT("16_Ww-Ew-"), TEXT("11_Oo") }, C.Fraction, 0.12f) &&
                (C.PoseID == TEXT("12_Ww-Oo-") || C.PoseID == TEXT("16_Ww-Ew-")))
            {
                continue;
            }

            if (C.Role == ESBVisualRole::Dominant)
            {
                SB_AddSpec(Specs, C.PoseID, C.Fraction, C.WidthFraction);
            }
            else
            {
                SB_AddSupportSpec(Specs, C.PoseID, C.Fraction, C.WidthFraction, C.Importance, C.SupportKind);
            }
        }
    }

    static void SB_CleanupSpecs(TArray<FSBSpec>& Specs)
    {
        for (int32 I = 0; I < Specs.Num(); ++I)
        {
            for (int32 J = I + 1; J < Specs.Num();)
            {
                const bool bSamePose = Specs[I].PoseID == Specs[J].PoseID;
                const bool bCollapsible = bSamePose && SB_IsVowelLikePose(Specs[I].PoseID) &&
                    FMath::Abs(Specs[I].Fraction - Specs[J].Fraction) <= 0.55f;
                if (!bCollapsible)
                {
                    ++J;
                    continue;
                }

                const float KeepI = SB_SpecImportanceForCleanup(Specs[I]) + (Specs[I].bHasLandmarkOverride && Specs[I].bLandmarkOverride ? 0.08f : 0.0f);
                const float KeepJ = SB_SpecImportanceForCleanup(Specs[J]) + (Specs[J].bHasLandmarkOverride && Specs[J].bLandmarkOverride ? 0.08f : 0.0f);
                if (KeepJ > KeepI)
                {
                    Specs[I] = Specs[J];
                }
                else
                {
                    Specs[I].WidthFraction = FMath::Max(Specs[I].WidthFraction, Specs[J].WidthFraction * 0.75f);
                }
                Specs.RemoveAt(J);
            }
        }
    }

    static void SB_ApplyGeneralizedVisibilityRules(const FString& Word, TArray<FSBSpec>& Specs)
    {
        const FString Key = SB_NormalizeTemplateWord(Word);
        if (Key.IsEmpty())
        {
            return;
        }

        SB_ApplyRoleCandidates(Key, Specs);
        SB_CleanupSpecs(Specs);
    }

    static TArray<FSBSpec> SB_SpecialSpecsForWord(const FString& Word)
    {
        const FString Key = SB_NormalizeTemplateWord(Word);
        TArray<FSBSpec> Specs;
        auto Add = [&Specs](const TCHAR* Pose, float Frac, float Width){ SB_AddSpec(Specs, FName(Pose), Frac, Width); };
        auto AddSupport = [&Specs](const TCHAR* Pose, float Frac, float Width, float Importance, const TCHAR* Kind)
        {
            SB_AddSupportSpec(Specs, FName(Pose), Frac, Width, Importance, FName(Kind));
        };
        if (Key == TEXT("hello")) { Add(TEXT("06_Eh"), .26f, .38f); Add(TEXT("09_Oh"), .72f, .44f); return Specs; }
        if (Key == TEXT("there")) { Add(TEXT("06_Eh"), .62f, .54f); return Specs; }
        if (Key == TEXT("that")) { Add(TEXT("07_Aa"), .62f, .54f); return Specs; }
        if (Key == TEXT("the")) { AddSupport(TEXT("06_Eh"), .62f, .26f, .44f, TEXT("terminal_reduction_support")); return Specs; }
        if (Key == TEXT("this")) { Add(TEXT("06_Eh"), .62f, .50f); return Specs; }
        if (Key == TEXT("to") || Key == TEXT("too") || Key == TEXT("two")) { Add(TEXT("11_Oo"), .62f, .38f); return Specs; }
        if (Key == TEXT("do")) { Add(TEXT("11_Oo"), .62f, .38f); return Specs; }
        if (Key == TEXT("you")) { AddSupport(TEXT("16_Ww-Ew-"), .18f, .18f, .66f, TEXT("template_protected")); Add(TEXT("11_Oo"), .60f, .58f); return Specs; }
        if (Key == TEXT("your")) { AddSupport(TEXT("16_Ww-Ew-"), .18f, .18f, .64f, TEXT("template_protected")); Add(TEXT("10_Or"), .62f, .54f); return Specs; }
        if (Key == TEXT("youre") || Key == TEXT("you're")) { AddSupport(TEXT("16_Ww-Ew-"), .16f, .18f, .64f, TEXT("template_protected")); Add(TEXT("10_Or"), .52f, .36f); return Specs; }
        if (Key == TEXT("we")) { Add(TEXT("12_Ww-Oo-"), .16f, .30f); Add(TEXT("03_Ee"), .58f, .36f); return Specs; }
        if (Key == TEXT("we've") || Key == TEXT("weve")) { AddSupport(TEXT("12_Ww-Oo-"), .16f, .14f, .52f, TEXT("round_glide_support")); Add(TEXT("20_FV"), .52f, .22f); return Specs; }
        if (Key == TEXT("we'll")) { AddSupport(TEXT("12_Ww-Oo-"), .16f, .16f, .50f, TEXT("round_glide_support")); Add(TEXT("03_Ee"), .54f, .30f); return Specs; }
        if (Key == TEXT("we're") || Key == TEXT("were")) { AddSupport(TEXT("16_Ww-Ew-"), .16f, .16f, .52f, TEXT("round_glide_support")); Add(TEXT("10_Or"), .54f, .36f); return Specs; }
        if (Key == TEXT("what")) { Add(TEXT("12_Ww-Oo-"), .14f, .34f); Add(TEXT("07_Aa"), .58f, .40f); return Specs; }
        if (Key == TEXT("what'll") || Key == TEXT("whatll")) { AddSupport(TEXT("12_Ww-Oo-"), .14f, .16f, .52f, TEXT("round_glide_support")); Add(TEXT("07_Aa"), .52f, .34f); return Specs; }
        // J6: /haʊ/ is a brief open lead-in glued to a dominant Oo target.
        // This prevents the Aa from firing early across the sentence boundary and
        // prevents a perceptual gap between the two halves of the diphthong.
        if (Key == TEXT("how")) { AddSupport(TEXT("07_Aa"), .58f, .16f, .46f, TEXT("GlideLeadIn")); Add(TEXT("11_Oo"), .74f, .44f); return Specs; }
        if (Key == TEXT("now")) { AddSupport(TEXT("07_Aa"), .58f, .16f, .46f, TEXT("GlideLeadIn")); Add(TEXT("11_Oo"), .74f, .44f); return Specs; }
        if (Key == TEXT("why")) { Add(TEXT("12_Ww-Oo-"), .14f, .34f); Add(TEXT("05_Ay"), .60f, .46f); return Specs; }
        if (Key == TEXT("which")) { Add(TEXT("12_Ww-Oo-"), .14f, .30f); Add(TEXT("14_ChJjSh"), .72f, .24f); return Specs; }
        if (Key == TEXT("i")) { Add(TEXT("05_Ay"), .56f, .48f); return Specs; }
        if (Key == TEXT("im") || Key == TEXT("i'm")) { Add(TEXT("04_Ih"), .30f, .38f); Add(TEXT("22_MBP"), .76f, .24f); return Specs; }
        if (Key == TEXT("today")) { Add(TEXT("11_Oo"), .34f, .30f); Add(TEXT("05_Ay"), .78f, .42f); return Specs; }

        // J11: runtime planner templates must live in the active SimpleBudget
        // path, not the retired Layer1 path above.  Keep these before the
        // representative-vowel fallback so multi-syllable/trajectory words
        // cannot collapse to one Eh/Ih event.
        if (Key == TEXT("about")) { AddSupport(TEXT("07_Aa"), .14f, .18f, .70f, TEXT("template_protected")); Add(TEXT("22_MBP"), .34f, .24f); Add(TEXT("11_Oo"), .72f, .42f); return Specs; }
        if (Key == TEXT("once")) { Add(TEXT("12_Ww-Oo-"), .14f, .32f); AddSupport(TEXT("07_Aa"), .54f, .34f, .72f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("story")) { Add(TEXT("14_ChJjSh"), .10f, .22f); Add(TEXT("10_Or"), .42f, .44f); AddSupport(TEXT("03_Ee"), .78f, .34f, .70f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("say") || Key == TEXT("says")) { Add(TEXT("05_Ay"), .62f, .48f); return Specs; }
        if (Key == TEXT("saying")) { AddSupport(TEXT("14_ChJjSh"), .12f, .22f, .54f, TEXT("sibilant_onset")); Add(TEXT("05_Ay"), .48f, .34f); AddSupport(TEXT("04_Ih"), .82f, .24f, .52f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("got")) { Add(TEXT("07_Aa"), .56f, .48f); return Specs; }
        if (Key == TEXT("pickles")) { Add(TEXT("22_MBP"), .08f, .24f); AddSupport(TEXT("04_Ih"), .42f, .30f, .60f, TEXT("template_protected")); Add(TEXT("14_ChJjSh"), .78f, .24f); return Specs; }
        if (Key == TEXT("thinking")) { AddSupport(TEXT("24_Tongue_Th"), .12f, .16f, .38f, TEXT("dental_support")); Add(TEXT("04_Ih"), .38f, .34f); AddSupport(TEXT("06_Eh"), .72f, .26f, .46f, TEXT("terminal_reduction_support")); return Specs; }
        if (Key == TEXT("forgiving")) { Add(TEXT("20_FV"), .10f, .24f); Add(TEXT("10_Or"), .30f, .34f); AddSupport(TEXT("04_Ih"), .52f, .26f, .60f, TEXT("template_protected")); Add(TEXT("20_FV"), .68f, .24f); AddSupport(TEXT("04_Ih"), .86f, .28f, .60f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("security")) { Add(TEXT("14_ChJjSh"), .10f, .22f); AddSupport(TEXT("06_Eh"), .28f, .30f, .68f, TEXT("template_protected")); AddSupport(TEXT("11_Oo"), .52f, .34f, .72f, TEXT("template_protected")); AddSupport(TEXT("03_Ee"), .82f, .34f, .78f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("interesting")) { Add(TEXT("04_Ih"), .14f, .24f); AddSupport(TEXT("06_Eh"), .38f, .20f, .48f, TEXT("medial_vowel_support")); AddSupport(TEXT("14_ChJjSh"), .60f, .18f, .48f, TEXT("internal_sibilant_support")); AddSupport(TEXT("04_Ih"), .84f, .24f, .52f, TEXT("terminal_reduction_support")); return Specs; }
        if (Key == TEXT("interested")) { Add(TEXT("04_Ih"), .14f, .24f); AddSupport(TEXT("06_Eh"), .38f, .20f, .48f, TEXT("medial_vowel_support")); AddSupport(TEXT("14_ChJjSh"), .58f, .18f, .48f, TEXT("internal_sibilant_support")); AddSupport(TEXT("06_Eh"), .82f, .24f, .52f, TEXT("terminal_reduction_support")); return Specs; }
        if (Key == TEXT("united")) { AddSupport(TEXT("16_Ww-Ew-"), .18f, .18f, .54f, TEXT("round_syllable_support")); Add(TEXT("05_Ay"), .48f, .34f); AddSupport(TEXT("06_Eh"), .82f, .22f, .44f, TEXT("terminal_reduction_support")); return Specs; }
        if (Key == TEXT("citizens")) { Add(TEXT("04_Ih"), .24f, .28f); AddSupport(TEXT("14_ChJjSh"), .52f, .18f, .48f, TEXT("internal_sibilant_support")); AddSupport(TEXT("06_Eh"), .80f, .22f, .48f, TEXT("terminal_reduction_support")); return Specs; }
        if (Key == TEXT("textiles")) { Add(TEXT("06_Eh"), .18f, .26f); Add(TEXT("05_Ay"), .52f, .28f); AddSupport(TEXT("14_ChJjSh"), .82f, .18f, .48f, TEXT("final_sibilant_support")); return Specs; }
        if (Key == TEXT("philosophy")) { Add(TEXT("20_FV"), .10f, .20f); Add(TEXT("04_Ih"), .28f, .24f); AddSupport(TEXT("07_Aa"), .48f, .24f, .52f, TEXT("medial_vowel_support")); AddSupport(TEXT("14_ChJjSh"), .64f, .16f, .44f, TEXT("internal_sibilant_support")); Add(TEXT("20_FV"), .82f, .20f); return Specs; }
        if (Key == TEXT("kilimanjaro")) { AddSupport(TEXT("04_Ih"), .18f, .22f, .52f, TEXT("medial_vowel_support")); AddSupport(TEXT("22_MBP"), .42f, .18f, .50f, TEXT("template_protected")); AddSupport(TEXT("14_ChJjSh"), .66f, .16f, .48f, TEXT("internal_sibilant_support")); Add(TEXT("09_Oh"), .84f, .26f); return Specs; }
        if (Key == TEXT("companions")) { Add(TEXT("22_MBP"), .18f, .24f); Add(TEXT("07_Aa"), .42f, .34f); Add(TEXT("14_ChJjSh"), .68f, .22f); AddSupport(TEXT("18_Uh"), .86f, .28f, .62f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("companion")) { Add(TEXT("22_MBP"), .18f, .24f); Add(TEXT("07_Aa"), .44f, .36f); Add(TEXT("14_ChJjSh"), .70f, .22f); return Specs; }
        if (Key == TEXT("partial")) { Add(TEXT("22_MBP"), .10f, .24f); Add(TEXT("07_Aa"), .36f, .34f); Add(TEXT("14_ChJjSh"), .68f, .22f); return Specs; }
        if (Key == TEXT("certain")) { Add(TEXT("14_ChJjSh"), .12f, .22f); AddSupport(TEXT("18_Uh"), .40f, .32f, .72f, TEXT("template_protected")); AddSupport(TEXT("06_Eh"), .76f, .30f, .64f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("customer")) { AddSupport(TEXT("18_Uh"), .24f, .32f, .70f, TEXT("template_protected")); Add(TEXT("14_ChJjSh"), .52f, .22f); Add(TEXT("22_MBP"), .78f, .24f); return Specs; }
        if (Key == TEXT("popular")) { Add(TEXT("22_MBP"), .12f, .24f); Add(TEXT("09_Oh"), .34f, .36f); AddSupport(TEXT("18_Uh"), .62f, .28f, .62f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("sourdough")) { Add(TEXT("14_ChJjSh"), .10f, .22f); Add(TEXT("10_Or"), .34f, .36f); Add(TEXT("11_Oo"), .72f, .40f); return Specs; }
        if (Key == TEXT("student")) { AddSupport(TEXT("14_ChJjSh"), .10f, .22f, .54f, TEXT("sibilant_onset")); AddSupport(TEXT("16_Ww-Ew-"), .34f, .22f, .54f, TEXT("round_glide_support")); Add(TEXT("11_Oo"), .56f, .34f); AddSupport(TEXT("24_Tongue_Th"), .84f, .18f, .40f, TEXT("front_stop_support")); return Specs; }
        if (Key == TEXT("sandwich")) { AddSupport(TEXT("14_ChJjSh"), .10f, .18f, .52f, TEXT("sibilant_onset")); Add(TEXT("07_Aa"), .36f, .30f); AddSupport(TEXT("12_Ww-Oo-"), .55f, .16f, .52f, TEXT("round_glide_support")); AddSupport(TEXT("14_ChJjSh"), .78f, .18f, .54f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("sandwiches")) { AddSupport(TEXT("14_ChJjSh"), .10f, .18f, .52f, TEXT("sibilant_onset")); Add(TEXT("07_Aa"), .34f, .28f); AddSupport(TEXT("12_Ww-Oo-"), .52f, .18f, .66f, TEXT("round_syllable_support")); AddSupport(TEXT("14_ChJjSh"), .72f, .18f, .54f, TEXT("template_protected")); AddSupport(TEXT("06_Eh"), .88f, .20f, .44f, TEXT("terminal_reduction_support")); return Specs; }
        if (Key == TEXT("community") || Key == TEXT("communities")) { Add(TEXT("22_MBP"), .26f, .22f); Add(TEXT("16_Ww-Ew-"), .48f, .24f); AddSupport(TEXT("18_Uh"), .66f, .18f, .34f, TEXT("terminal_reduction_support")); Add(TEXT("03_Ee"), .84f, .26f); return Specs; }
        if (Key == TEXT("effort")) { Add(TEXT("06_Eh"), .18f, .28f); Add(TEXT("20_FV"), .42f, .20f); Add(TEXT("10_Or"), .74f, .30f); return Specs; }
        if (Key == TEXT("spread")) { AddSupport(TEXT("14_ChJjSh"), .10f, .16f, .44f, TEXT("sibilant_onset")); Add(TEXT("22_MBP"), .30f, .20f); Add(TEXT("06_Eh"), .62f, .38f); return Specs; }
        if (Key == TEXT("snake")) { AddSupport(TEXT("14_ChJjSh"), .10f, .22f, .54f, TEXT("sibilant_onset")); Add(TEXT("05_Ay"), .58f, .42f); return Specs; }
        if (Key == TEXT("steak")) { AddSupport(TEXT("14_ChJjSh"), .10f, .22f, .54f, TEXT("sibilant_onset")); Add(TEXT("05_Ay"), .58f, .42f); return Specs; }
        if (Key == TEXT("sorry")) { AddSupport(TEXT("14_ChJjSh"), .10f, .22f, .54f, TEXT("sibilant_onset")); Add(TEXT("10_Or"), .42f, .34f); AddSupport(TEXT("03_Ee"), .78f, .28f, .52f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("animal")) { Add(TEXT("07_Aa"), .18f, .30f); Add(TEXT("22_MBP"), .42f, .22f); AddSupport(TEXT("06_Eh"), .66f, .26f, .52f, TEXT("medial_vowel_support")); AddSupport(TEXT("24_Tongue_Th"), .86f, .18f, .34f, TEXT("front_stop_support")); return Specs; }
        if (Key == TEXT("mammary")) { Add(TEXT("22_MBP"), .10f, .24f); Add(TEXT("07_Aa"), .32f, .30f); AddSupport(TEXT("22_MBP"), .52f, .18f, .50f, TEXT("template_protected")); Add(TEXT("10_Or"), .70f, .26f); AddSupport(TEXT("03_Ee"), .86f, .22f, .50f, TEXT("medial_vowel_support")); return Specs; }
        if (Key == TEXT("poisonous")) { Add(TEXT("22_MBP"), .10f, .22f); Add(TEXT("09_Oh"), .34f, .26f); AddSupport(TEXT("05_Ay"), .52f, .22f, .52f, TEXT("medial_vowel_support")); AddSupport(TEXT("18_Uh"), .72f, .24f, .48f, TEXT("medial_vowel_support")); Add(TEXT("14_ChJjSh"), .86f, .18f); return Specs; }
        if (Key == TEXT("extinguisher")) { AddSupport(TEXT("06_Eh"), .12f, .22f, .54f, TEXT("medial_vowel_support")); AddSupport(TEXT("14_ChJjSh"), .30f, .20f, .54f, TEXT("sibilant_onset")); AddSupport(TEXT("04_Ih"), .46f, .22f, .50f, TEXT("medial_vowel_support")); AddSupport(TEXT("16_Ww-Ew-"), .64f, .22f, .54f, TEXT("round_glide_support")); AddSupport(TEXT("14_ChJjSh"), .78f, .18f, .56f, TEXT("template_protected")); AddSupport(TEXT("10_Or"), .90f, .20f, .52f, TEXT("medial_vowel_support")); return Specs; }
        if (Key == TEXT("lizard")) { AddSupport(TEXT("04_Ih"), .22f, .24f, .52f, TEXT("medial_vowel_support")); AddSupport(TEXT("14_ChJjSh"), .46f, .20f, .54f, TEXT("sibilant_onset")); AddSupport(TEXT("10_Or"), .78f, .24f, .50f, TEXT("medial_vowel_support")); return Specs; }
        if (Key == TEXT("hearty")) { Add(TEXT("07_Aa"), .28f, .32f); Add(TEXT("10_Or"), .58f, .28f); AddSupport(TEXT("03_Ee"), .84f, .22f, .48f, TEXT("medial_vowel_support")); return Specs; }
        if (Key == TEXT("and")) { AddSupport(TEXT("18_Uh"), .46f, .28f, .52f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("is")) { Add(TEXT("04_Ih"), .38f, .28f); AddSupport(TEXT("14_ChJjSh"), .78f, .18f, .52f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("has")) { AddSupport(TEXT("18_Uh"), .42f, .28f, .52f, TEXT("template_protected")); AddSupport(TEXT("14_ChJjSh"), .78f, .18f, .52f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("have")) { Add(TEXT("20_FV"), .14f, .22f); AddSupport(TEXT("18_Uh"), .52f, .26f, .54f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("had")) { AddSupport(TEXT("18_Uh"), .44f, .28f, .50f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("can")) { AddSupport(TEXT("18_Uh"), .48f, .28f, .50f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("cant") || Key == TEXT("can't")) { Add(TEXT("07_Aa"), .46f, .34f); return Specs; }
        if (Key == TEXT("dont") || Key == TEXT("don't")) { Add(TEXT("09_Oh"), .52f, .38f); return Specs; }
        if (Key == TEXT("in")) { Add(TEXT("04_Ih"), .38f, .26f); return Specs; }
        if (Key == TEXT("it")) { Add(TEXT("04_Ih"), .38f, .26f); return Specs; }
        if (Key == TEXT("was")) { AddSupport(TEXT("12_Ww-Oo-"), .16f, .18f, .60f, TEXT("template_protected")); Add(TEXT("18_Uh"), .54f, .30f); AddSupport(TEXT("14_ChJjSh"), .84f, .18f, .48f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("were")) { AddSupport(TEXT("16_Ww-Ew-"), .18f, .18f, .60f, TEXT("template_protected")); Add(TEXT("10_Or"), .60f, .44f); return Specs; }
        if (Key == TEXT("of")) { Add(TEXT("18_Uh"), .42f, .26f); Add(TEXT("20_FV"), .78f, .20f); return Specs; }
        if (Key == TEXT("be")) { Add(TEXT("22_MBP"), .18f, .22f); Add(TEXT("03_Ee"), .66f, .42f); return Specs; }
        if (Key == TEXT("home")) { Add(TEXT("11_Oo"), .54f, .40f); AddSupport(TEXT("22_MBP"), .84f, .18f, .36f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("known")) { Add(TEXT("09_Oh"), .48f, .34f); return Specs; }
        if (Key == TEXT("only")) { Add(TEXT("09_Oh"), .26f, .30f); AddSupport(TEXT("18_Uh"), .58f, .22f, .46f, TEXT("template_protected")); Add(TEXT("03_Ee"), .82f, .30f); return Specs; }
        if (Key == TEXT("right")) { Add(TEXT("05_Ay"), .54f, .44f); return Specs; }
        if (Key == TEXT("great")) { Add(TEXT("05_Ay"), .52f, .42f); return Specs; }
        if (Key == TEXT("heard")) { Add(TEXT("10_Or"), .52f, .38f); return Specs; }
        if (Key == TEXT("learn")) { Add(TEXT("10_Or"), .48f, .34f); return Specs; }
        if (Key == TEXT("world")) { AddSupport(TEXT("16_Ww-Ew-"), .18f, .18f, .58f, TEXT("template_protected")); Add(TEXT("10_Or"), .56f, .42f); return Specs; }
        if (Key == TEXT("work") || Key == TEXT("word")) { AddSupport(TEXT("16_Ww-Ew-"), .18f, .18f, .58f, TEXT("template_protected")); Add(TEXT("10_Or"), .56f, .42f); return Specs; }
        if (Key == TEXT("person")) { Add(TEXT("22_MBP"), .12f, .22f); Add(TEXT("10_Or"), .42f, .30f); AddSupport(TEXT("14_ChJjSh"), .72f, .18f, .40f, TEXT("template_protected")); return Specs; }
        if (Key == TEXT("enjoy")) { Add(TEXT("14_ChJjSh"), .14f, .20f); Add(TEXT("09_Oh"), .52f, .30f); Add(TEXT("05_Ay"), .82f, .28f); return Specs; }

        if (Key == TEXT("welcome")) { Add(TEXT("12_Ww-Oo-"), .12f, .18f); Add(TEXT("06_Eh"), .44f, .22f); Add(TEXT("07_Aa"), .76f, .18f); Add(TEXT("22_MBP"), .90f, .20f); return Specs; }
        if (Key == TEXT("alfie") || Key == TEXT("alfie's") || Key == TEXT("alfies"))
        {
            Add(TEXT("07_Aa"), .16f, .30f);
            Add(TEXT("20_FV"), .54f, .24f);
            Add(TEXT("03_Ee"), .78f, .42f);
            return Specs;
        }
        if (Key == TEXT("bodega")) { Add(TEXT("22_MBP"), .08f, .24f); Add(TEXT("09_Oh"), .30f, .28f); Add(TEXT("05_Ay"), .58f, .22f); Add(TEXT("07_Aa"), .82f, .30f); return Specs; }
        if (Key == TEXT("matthew")) { Add(TEXT("22_MBP"), .10f, .24f); Add(TEXT("24_Tongue_Th"), .55f, .24f); Add(TEXT("12_Ww-Oo-"), .78f, .36f); return Specs; }
        return Specs;
    }

    static float SB_SpecComplexityUnitBonus(const TArray<FSBSpec>& Specs)
    {
        if (Specs.Num() <= 0)
        {
            return 0.0f;
        }

        int32 LandmarkCount = 0;
        int32 StrongSupportCount = 0;
        bool bHasEarly = false;
        bool bHasLate = false;
        for (const FSBSpec& Spec : Specs)
        {
            LandmarkCount += SB_IsLandmarkPose(Spec.PoseID) ? 1 : 0;
            StrongSupportCount += Spec.SupportKind == TEXT("template_protected") ? 1 : 0;
            bHasEarly = bHasEarly || Spec.Fraction <= 0.28f;
            bHasLate = bHasLate || Spec.Fraction >= 0.72f;
        }

        float Bonus = 0.10f * static_cast<float>(FMath::Max(0, Specs.Num() - 1));
        Bonus += 0.035f * static_cast<float>(FMath::Max(0, LandmarkCount - 1));
        Bonus += 0.025f * static_cast<float>(StrongSupportCount);
        if (bHasEarly && bHasLate && Specs.Num() >= 3)
        {
            Bonus += 0.06f;
        }
        return FMath::Clamp(Bonus, 0.0f, 0.42f);
    }

    static float SB_RefineEstimatedDurationWithEvents(const FOffgridAITextVisemePlan& Plan, float BaseDurationSec)
    {
        // Keep the text duration speech-only, but do not let the visible event
        // queue become shorter than its own monotonic readability budget.
        //
        // This is not a pause model and not a second timing authority. It is a
        // liveness floor derived from the thing we actually must perform: every
        // planned viseme, in order. Audio island starts still decide phrase
        // launch timing; this floor only prevents the line from exhausting all
        // events halfway through the spoken audio.
        int32 LandmarkCount = 0;
        int32 StrongCount = 0;
        int32 ReducedCount = 0;
        for (const FOffgridAITextVisemeEvent& Event : Plan.Events)
        {
            LandmarkCount += Event.bIsLandmark ? 1 : 0;
            StrongCount += Event.Strength >= 0.68f ? 1 : 0;
            ReducedCount += Event.bIsFunctionWord ? 1 : 0;
        }

        const int32 EventCount = Plan.Events.Num();
        if (EventCount <= 0)
        {
            return BaseDurationSec;
        }

        // Minimum readable centers for a monotonic stream:
        // - most events need roughly 85 ms center-to-center to be perceived
        // - landmarks and strong shapes need a little more occupancy
        // - reduced/function-word support shapes should not bloat the line
        const float EventFloor =
            0.18f
            + 0.085f * static_cast<float>(EventCount)
            + 0.018f * static_cast<float>(LandmarkCount)
            + 0.010f * static_cast<float>(StrongCount)
            - 0.010f * static_cast<float>(ReducedCount);

        return FMath::Clamp(FMath::Max(BaseDurationSec, EventFloor), 0.45f, 10.5f);
    }


    static FName SB_DominantVowelPoseForWord(const FString& Word)
    {
        const FString Key = SB_NormalizeTemplateWord(Word);
        if (Key == TEXT("you") || Key == TEXT("too") || Key == TEXT("do") || Key == TEXT("food") || Key == TEXT("move") || Key == TEXT("blue") || Key == TEXT("soon")) { return TEXT("11_Oo"); }
        if (Key == TEXT("your") || Key == TEXT("or") || Key == TEXT("for") || Key == TEXT("store") || Key == TEXT("four") || Key == TEXT("warmth")) { return TEXT("10_Or"); }
        if (Key == TEXT("home") || Key == TEXT("known") || Key == TEXT("only") || Key == TEXT("old") || Key == TEXT("most")) { return TEXT("09_Oh"); }
        if (Key == TEXT("right") || Key == TEXT("find") || Key == TEXT("time") || Key == TEXT("line")) { return TEXT("05_Ay"); }
        if (Key == TEXT("great") || Key == TEXT("day") || Key == TEXT("name")) { return TEXT("05_Ay"); }
        if (Key == TEXT("heard") || Key == TEXT("learn") || Key == TEXT("world") || Key == TEXT("work") || Key == TEXT("word") || Key == TEXT("person")) { return TEXT("10_Or"); }
        if (Key == TEXT("enjoy") || Key == TEXT("boy") || Key == TEXT("voice")) { return TEXT("09_Oh"); }
        if (Key == TEXT("and") || Key == TEXT("can") || Key == TEXT("has") || Key == TEXT("have") || Key == TEXT("was") || Key == TEXT("of")) { return TEXT("18_Uh"); }
        if (Key == TEXT("is") || Key == TEXT("in") || Key == TEXT("it")) { return TEXT("04_Ih"); }
        if (Key == TEXT("spread") || Key == TEXT("bread") || Key == TEXT("dread") || Key == TEXT("thread") || Key == TEXT("head") || Key == TEXT("ready")) { return TEXT("06_Eh"); }
        if (SB_EndsWithAny(Key, { TEXT("ake"), TEXT("ate"), TEXT("ail"), TEXT("ain"), TEXT("ame"), TEXT("ase"), TEXT("eak"), TEXT("eigh") })) { return TEXT("05_Ay"); }
        if (Key.EndsWith(TEXT("aying"))) { return TEXT("05_Ay"); }
        if (SB_EndsWithAny(Key, { TEXT("ight"), TEXT("ind"), TEXT("ime"), TEXT("ine"), TEXT("ire") })) { return TEXT("05_Ay"); }
        if (Key.Len() <= 5 && Key.EndsWith(TEXT("e")) && Key.Contains(TEXT("i")) && !SB_ContainsAny(Key, { TEXT("ie"), TEXT("ee") })) { return TEXT("05_Ay"); }
        if (SB_EndsWithAny(Key, { TEXT("ome"), TEXT("one"), TEXT("old"), TEXT("olt"), TEXT("ost") })) { return TEXT("09_Oh"); }
        if (SB_ContainsAny(Key, { TEXT("oy"), TEXT("oi") })) { return TEXT("09_Oh"); }
        if (SB_ContainsAny(Key, { TEXT("ear"), TEXT("eer"), TEXT("ere"), TEXT("ir"), TEXT("ur") })) { return TEXT("10_Or"); }
        if (SB_StartsWithAny(Key, { TEXT("stu"), TEXT("tu"), TEXT("du"), TEXT("nu"), TEXT("mu"), TEXT("hu") })) { return TEXT("11_Oo"); }
        if (Key.Len() >= 3 && Key.EndsWith(TEXT("e")) && Key.Contains(TEXT("o")) && !SB_ContainsAny(Key, { TEXT("ee"), TEXT("ie") })) { return TEXT("09_Oh"); }
        if (Key.Contains(TEXT("oo")) || Key.Contains(TEXT("ue")) || Key.Contains(TEXT("ui"))) { return TEXT("11_Oo"); }
        if (Key.Contains(TEXT("ew"))) { return TEXT("16_Ww-Ew-"); }
        if (Key.Contains(TEXT("ow")) || Key.Contains(TEXT("ou")) || Key.EndsWith(TEXT("o"))) { return TEXT("09_Oh"); }
        if (Key.Contains(TEXT("ee")) || Key.Contains(TEXT("ea")) || Key.Contains(TEXT("ie")) || Key.EndsWith(TEXT("y"))) { return TEXT("03_Ee"); }
        if (Key == TEXT("i")) { return TEXT("05_Ay"); }
        if (Key.Contains(TEXT("ai")) || Key.Contains(TEXT("ay"))) { return TEXT("05_Ay"); }
        if (Key.Contains(TEXT("a"))) { return TEXT("07_Aa"); }
        if (Key.Contains(TEXT("e"))) { return TEXT("06_Eh"); }
        if (Key.Contains(TEXT("i"))) { return TEXT("04_Ih"); }
        if (Key.Contains(TEXT("o"))) { return TEXT("09_Oh"); }
        if (Key.Contains(TEXT("u"))) { return TEXT("18_Uh"); }
        return TEXT("06_Eh");
    }

    static bool SB_NeedsFrontOnset(const FString& Word)
    {
        if (Word.StartsWith(TEXT("th")) || Word.IsEmpty() || Word[0] != TEXT('t')) { return false; }
        return Word == TEXT("to") || Word == TEXT("too") || Word == TEXT("two") || Word == TEXT("today") || Word == TEXT("tell") || Word == TEXT("take");
    }

    static bool SB_NeedsLiquidFrontTravel(const FString& Word)
    {
        return Word.StartsWith(TEXT("wel")) || Word.StartsWith(TEXT("hel")) || Word.StartsWith(TEXT("whol")) || Word.StartsWith(TEXT("well"))
            || Word.StartsWith(TEXT("alf")) || Word.StartsWith(TEXT("alph")) || Word.StartsWith(TEXT("alb")) || Word.StartsWith(TEXT("al"))
            || Word.StartsWith(TEXT("flav")) || Word.StartsWith(TEXT("friendl"));
    }

    static FName SB_VowelPoseForGroup(const FString& Key, int32 GroupStart, int32 GroupEndExclusive)
    {
        if (GroupStart < 0 || GroupEndExclusive <= GroupStart || GroupEndExclusive > Key.Len())
        {
            return NAME_None;
        }

        const FString Group = Key.Mid(GroupStart, GroupEndExclusive - GroupStart);
        const TCHAR Next = GroupEndExclusive < Key.Len() ? Key[GroupEndExclusive] : 0;
        if (Group.Contains(TEXT("oo")) || Group.Contains(TEXT("ue")) || Group.Contains(TEXT("ui"))) { return TEXT("11_Oo"); }
        if (Group.Contains(TEXT("ea")) && (Next == TEXT('k') || Next == TEXT('t') || Next == TEXT('d'))) { return TEXT("05_Ay"); }
        if (Group.Contains(TEXT("a")) && Next == TEXT('k') && Key.EndsWith(TEXT("e"))) { return TEXT("05_Ay"); }
        if (Group.Contains(TEXT("ew"))) { return TEXT("16_Ww-Ew-"); }
        if (Group.Contains(TEXT("ee")) || Group.Contains(TEXT("ea")) || Group.Contains(TEXT("ie")) || (Group.EndsWith(TEXT("y")) && GroupStart > 0)) { return TEXT("03_Ee"); }
        if (Group.Contains(TEXT("ai")) || Group.Contains(TEXT("ay")) || Group.Contains(TEXT("ei")) || Group.Contains(TEXT("igh"))) { return TEXT("05_Ay"); }
        if (Group.Contains(TEXT("oy")) || Group.Contains(TEXT("oi"))) { return TEXT("09_Oh"); }
        if (Group.Contains(TEXT("ow")) || Group.Contains(TEXT("ou")) || Group.Contains(TEXT("oa"))) { return TEXT("09_Oh"); }
        if (Group.Contains(TEXT("or")) || (Group.Contains(TEXT("o")) && Next == TEXT('r'))) { return TEXT("10_Or"); }
        if (Group.Contains(TEXT("ir")) || Group.Contains(TEXT("ur")) || Group.Contains(TEXT("ear")) || Group.Contains(TEXT("ere"))) { return TEXT("10_Or"); }
        if (Group.Contains(TEXT("a"))) { return TEXT("07_Aa"); }
        if (Group.Contains(TEXT("e"))) { return TEXT("06_Eh"); }
        if (Group.Contains(TEXT("i"))) { return TEXT("04_Ih"); }
        if (Group.Contains(TEXT("o"))) { return TEXT("09_Oh"); }
        if (Group.Contains(TEXT("u"))) { return TEXT("18_Uh"); }
        if (Group.Contains(TEXT("y"))) { return TEXT("03_Ee"); }
        return NAME_None;
    }

    static TArray<FSBVowelAnchor> SB_CollectVowelAnchors(const FString& Word)
    {
        TArray<FSBVowelAnchor> Out;
        const FString Key = SB_NormalizeTemplateWord(Word);
        if (Key.IsEmpty())
        {
            return Out;
        }

        int32 I = 0;
        while (I < Key.Len())
        {
            if (!IsVowelChar(Key[I]))
            {
                ++I;
                continue;
            }

            const int32 GroupStart = I;
            while (I < Key.Len() && IsVowelChar(Key[I]))
            {
                ++I;
            }
            const int32 GroupEndExclusive = I;
            const bool bLooksSilentFinalE = GroupEndExclusive == Key.Len() &&
                GroupEndExclusive - GroupStart == 1 &&
                Key[GroupStart] == TEXT('e') &&
                Key.Len() > 3 &&
                GroupStart > 0 &&
                !IsVowelChar(Key[GroupStart - 1]);
            if (bLooksSilentFinalE)
            {
                continue;
            }

            const FName PoseID = SB_VowelPoseForGroup(Key, GroupStart, GroupEndExclusive);
            if (PoseID == NAME_None)
            {
                continue;
            }

            FSBVowelAnchor Anchor;
            Anchor.PoseID = PoseID;
            Anchor.Fraction = FMath::Clamp((static_cast<float>(GroupStart + GroupEndExclusive) * 0.5f) / static_cast<float>(Key.Len()), 0.08f, 0.92f);
            Out.Add(Anchor);
        }
        return Out;
    }

    static TArray<FSBSpec> SB_GenericSpecsForWord(const FString& Word)
    {
        TArray<FSBSpec> Specs;
        if (Word.IsEmpty()) { return Specs; }
        const int32 N = FMath::Max(1, Word.Len());
        if (SB_NeedsPlainSibilantOnset(Word)
            || Word.StartsWith(TEXT("z"))
            || SB_StartsWithAny(Word, { TEXT("sc"), TEXT("sk"), TEXT("sl"), TEXT("sm"), TEXT("sn"), TEXT("sp"), TEXT("st"), TEXT("sw") }))
        {
            SB_AddSupportSpec(Specs, TEXT("14_ChJjSh"), .10f, .20f, .58f, TEXT("sibilant_onset"));
        }
        if (Word.StartsWith(TEXT("th"))) { SB_AddSupportSpec(Specs, TEXT("24_Tongue_Th"), .12f, .16f, .40f, TEXT("dental_support")); }
        else if (Word.StartsWith(TEXT("ch")) || Word.StartsWith(TEXT("sh")) || Word.StartsWith(TEXT("j"))) { SB_AddSpec(Specs, TEXT("14_ChJjSh"), .12f, .24f); }
        else if (Word.StartsWith(TEXT("wh")) || Word.StartsWith(TEXT("w"))) { SB_AddSpec(Specs, TEXT("12_Ww-Oo-"), .14f, .32f); }
        else if (SB_NeedsFrontOnset(Word)) { SB_AddSupportSpec(Specs, TEXT("24_Tongue_Th"), .12f, .16f, .52f, TEXT("front_stop_support")); }
        else if (Word[0] == TEXT('m') || Word[0] == TEXT('b') || Word[0] == TEXT('p')) { SB_AddSpec(Specs, TEXT("22_MBP"), .10f, .24f); }
        else if (Word[0] == TEXT('f') || Word[0] == TEXT('v')) { SB_AddSpec(Specs, TEXT("20_FV"), .12f, .24f); }
        if (SB_StartsWithAny(Word, { TEXT("stu"), TEXT("tu"), TEXT("du"), TEXT("nu"), TEXT("mu"), TEXT("hu") }))
        {
            SB_AddSupportSpec(Specs, TEXT("16_Ww-Ew-"), .34f, .20f, .56f, TEXT("round_glide_support"));
        }

        if (Word.StartsWith(TEXT("al")) && (Word.Len() == 2 || !IsVowelChar(Word[2])))
        {
            SB_AddSpec(Specs, TEXT("07_Aa"), .16f, .28f);
        }

        const int32 LIndex = Word.Find(TEXT("l"), ESearchCase::IgnoreCase, ESearchDir::FromStart, 1);
        if (LIndex > 0 && SB_NeedsLiquidFrontTravel(Word))
        {
        }

        const FName Dominant = SB_DominantVowelPoseForWord(Word);
        bool bAlreadyHasDominant = false;
        for (const FSBSpec& S : Specs) { if (S.PoseID == Dominant) { bAlreadyHasDominant = true; break; } }
        if (!bAlreadyHasDominant)
        {
            SB_AddSpec(Specs, Dominant, .58f, SB_IsFunctionOrReducedWord(Word) ? .34f : .52f);
        }

        auto AddFirstInterior = [&Specs, &Word, N](const FString& Chars, const TCHAR* Pose, float Bias)
        {
            int32 Best = INDEX_NONE;
            for (TCHAR C : Chars)
            {
                const int32 At = Word.Find(FString::Chr(C), ESearchCase::IgnoreCase, ESearchDir::FromStart, 1);
                if (At > 0 && (Best == INDEX_NONE || At < Best)) { Best = At; }
            }
            if (Best > 0)
            {
                SB_AddSpec(Specs, FName(Pose), FMath::Clamp((static_cast<float>(Best) + Bias) / static_cast<float>(N), .08f, .92f), .24f);
            }
        };
        AddFirstInterior(TEXT("mbp"), TEXT("22_MBP"), .50f);
        AddFirstInterior(TEXT("fv"), TEXT("20_FV"), .50f);
        const int32 Th = Word.Find(TEXT("th"), ESearchCase::IgnoreCase, ESearchDir::FromStart, 1);
        if (Th > 0) { SB_AddSpec(Specs, TEXT("24_Tongue_Th"), FMath::Clamp((static_cast<float>(Th) + .5f) / static_cast<float>(N), .08f, .92f), .24f); }
        const int32 Sh = Word.Find(TEXT("sh"), ESearchCase::IgnoreCase, ESearchDir::FromStart, 1);
        const int32 Ch = Word.Find(TEXT("ch"), ESearchCase::IgnoreCase, ESearchDir::FromStart, 1);
        const int32 Sibilant = (Sh > 0 && Ch > 0) ? FMath::Min(Sh, Ch) : FMath::Max(Sh, Ch);
        if (Sibilant > 0) { SB_AddSpec(Specs, TEXT("14_ChJjSh"), FMath::Clamp((static_cast<float>(Sibilant) + .6f) / static_cast<float>(N), .10f, .90f), .24f); }
        if (SB_EndsWithAny(Word, { TEXT("s"), TEXT("z"), TEXT("ce"), TEXT("se") }) && !SB_ContainsAny(Word, { TEXT("sh"), TEXT("ch") }))
        {
            SB_AddSupportSpec(Specs, TEXT("14_ChJjSh"), 0.84f, 0.18f, 0.44f, TEXT("template_protected"));
        }

        // J11: active-path generic long-word preservation. If no explicit
        // template exists, long/multi-syllable content should still get a
        // compact trajectory rather than a single representative vowel.
        const int32 Syllables = EstimateSyllableCount(Word);
        if ((Word.Len() >= 8 || Syllables >= 3) && Specs.Num() < 3)
        {
            const FName LongWordDominant = SB_DominantVowelPoseForWord(Word);
            bool bHasEarly = false;
            bool bHasLate = false;
            for (const FSBSpec& S : Specs)
            {
                bHasEarly = bHasEarly || S.Fraction <= 0.30f;
                bHasLate = bHasLate || S.Fraction >= 0.70f;
            }
            if (!bHasEarly)
            {
                SB_AddSupportSpec(Specs, Word.StartsWith(TEXT("in")) ? FName(TEXT("04_Ih")) : LongWordDominant, .18f, .26f, .68f, TEXT("template_protected"));
            }
            if (Word.Contains(TEXT("st")) || Word.Contains(TEXT("s")))
            {
                SB_AddSupportSpec(Specs, TEXT("14_ChJjSh"), .54f, .18f, .48f, TEXT("internal_sibilant_support"));
            }
            else if (Word.Contains(TEXT("t")) || Word.Contains(TEXT("d")))
            {
                SB_AddSpec(Specs, TEXT("24_Tongue_Th"), .52f, .22f);
            }
            else if (Word.Contains(TEXT("r")))
            {
                SB_AddSpec(Specs, TEXT("10_Or"), .52f, .28f);
            }
            if (!bHasLate && Specs.Num() < 4)
            {
                SB_AddSupportSpec(Specs, Word.EndsWith(TEXT("ing")) ? FName(TEXT("04_Ih")) : Dominant, .82f, .30f, .64f, TEXT("template_protected"));
            }
        }

        const bool bContentWord = IsMeaningfulWord(Word) && !SB_IsFunctionOrReducedWord(Word);
        const TArray<FSBVowelAnchor> VowelAnchors = SB_CollectVowelAnchors(Word);
        if (bContentWord && VowelAnchors.Num() >= 2)
        {
            auto HasSpecNear = [&Specs](float Fraction)
            {
                for (const FSBSpec& Existing : Specs)
                {
                    if (FMath::Abs(Existing.Fraction - Fraction) <= 0.12f)
                    {
                        return true;
                    }
                }
                return false;
            };

            int32 AddedSupports = 0;
            for (int32 AnchorIndex = 0; AnchorIndex < VowelAnchors.Num(); ++AnchorIndex)
            {
                const FSBVowelAnchor& Anchor = VowelAnchors[AnchorIndex];
                const bool bTerminalAnchor = AnchorIndex == VowelAnchors.Num() - 1;
                const bool bEarlyAnchor = AnchorIndex == 0;
                const bool bMedialAnchor = !bTerminalAnchor && !bEarlyAnchor;
                if (bMedialAnchor && (VowelAnchors.Num() < 3 || Word.Len() < 7 || AnchorIndex != VowelAnchors.Num() / 2))
                {
                    continue;
                }
                if (!bMedialAnchor && !bTerminalAnchor && !bEarlyAnchor)
                {
                    continue;
                }
                if (HasSpecNear(Anchor.Fraction))
                {
                    continue;
                }

                const bool bWeakTerminalReduction = bTerminalAnchor &&
                    (Word.EndsWith(TEXT("es")) || Word.EndsWith(TEXT("ed")) || Word.EndsWith(TEXT("er")) || Word.EndsWith(TEXT("ers")));
                const float Width = bMedialAnchor ? 0.24f : (bWeakTerminalReduction ? 0.22f : (bTerminalAnchor ? 0.32f : 0.28f));
                const float Importance = bMedialAnchor ? 0.52f : (bWeakTerminalReduction ? 0.48f : (bTerminalAnchor ? 0.72f : 0.68f));
                SB_AddSupportSpec(
                    Specs,
                    Anchor.PoseID,
                    Anchor.Fraction,
                    Width,
                    Importance,
                    bWeakTerminalReduction ? TEXT("terminal_reduction_support") : (bMedialAnchor ? TEXT("medial_vowel_support") : TEXT("template_protected")));
                ++AddedSupports;
                if (AddedSupports >= (VowelAnchors.Num() >= 3 ? 3 : 2))
                {
                    break;
                }
            }
        }
        return Specs;
    }

    static FName SB_SupportClass(const FSBSpec& Spec, bool bIsLandmark)
    {
        if (bIsLandmark) { return TEXT("landmark"); }
        if (Spec.SupportKind != NAME_None) { return Spec.SupportKind; }
        const FString Pose = Spec.PoseID.ToString();
        if (Pose == TEXT("03_Ee") || Pose == TEXT("04_Ih") || Pose == TEXT("05_Ay") || Pose == TEXT("06_Eh")) { return TEXT("front_vowel_support"); }
        if (Pose == TEXT("09_Oh") || Pose == TEXT("10_Or") || Pose == TEXT("18_Uh")) { return TEXT("relaxed_round_support"); }
        if (Pose == TEXT("11_Oo") || Pose == TEXT("12_Ww-Oo-") || Pose == TEXT("16_Ww-Ew-")) { return TEXT("round_support"); }
        if (Pose == TEXT("07_Aa") || Pose == TEXT("08_Ah")) { return TEXT("open_support"); }
        return TEXT("other_support");
    }

    static float SB_ClassPressureLoss(FName SupportClass)
    {
        if (SupportClass == TEXT("landmark")) { return 0.18f; }
        if (SupportClass == TEXT("stressed_vowel")) { return 0.16f; }
        if (SupportClass == TEXT("template_protected")) { return 0.16f; }
        if (SupportClass == TEXT("sibilant_onset")) { return 0.24f; }
        if (SupportClass == TEXT("internal_sibilant_support")) { return 0.34f; }
        if (SupportClass == TEXT("final_sibilant_support")) { return 0.30f; }
        if (SupportClass == TEXT("round_glide_support")) { return 0.30f; }
        if (SupportClass == TEXT("round_syllable_support")) { return 0.24f; }
        if (SupportClass == TEXT("rhotic_support")) { return 0.36f; }
        if (SupportClass == TEXT("dental_support")) { return 0.42f; }
        if (SupportClass == TEXT("final_stop_support")) { return 0.50f; }
        if (SupportClass == TEXT("medial_vowel_support")) { return 0.54f; }
        if (SupportClass == TEXT("terminal_reduction_support")) { return 0.58f; }
        if (SupportClass == TEXT("front_stop_support")) { return 0.36f; }
        if (SupportClass == TEXT("liquid_front_travel")) { return 0.50f; }
        if (SupportClass == TEXT("front_vowel_support")) { return 0.46f; }
        if (SupportClass == TEXT("relaxed_round_support")) { return 0.50f; }
        if (SupportClass == TEXT("round_support")) { return 0.42f; }
        if (SupportClass == TEXT("open_support")) { return 0.38f; }
        return 0.44f;
    }

    struct FSBIntent
    {
        FName PoseID = NAME_None;
        float CenterNorm = 0.0f;
        float StartNorm = 0.0f;
        float EndNorm = 0.0f;
        FString SourceWord;
        int32 WordIndex = INDEX_NONE;
        int32 PhraseIndex = 0;
        int32 SentenceIslandIndex = 0;
        int32 PhraseWordCount = 1;
        int32 PhraseWordFromEnd = 0;
        float PhraseSpanNorm = 0.0f;
        bool bIsLandmark = false;
        bool bIsReduced = false;
        float BaseImportance = 0.55f;
        float ArticulationPressure = 0.0f;
        float Realization = 0.0f;
        FName SupportKind = NAME_None;
        FName SupportClass = NAME_None;
    };

FOffgridAITextVisemePlan FOffgridAITextVisemePlanner::BuildPlan(const FText& Dialogue, float CharactersPerSecond, float MinDurationSeconds)
{
    FOffgridAITextVisemePlan Plan;
    const FString OriginalText = Dialogue.ToString();
    const FString Text = OriginalText.ToLower();
    const TArray<FWordSpan> Words = TokenizeWords(Text);
    const TArray<FWordSpan> OriginalWords = TokenizeWords(OriginalText);

    if (Words.Num() <= 0)
    {
        Plan.EstimatedDurationSeconds = SB_EstimateDurationSeconds(Text, Words, &OriginalWords, MinDurationSeconds);
        return Plan;
    }

    Plan.WordSentenceIslandIndices.Reserve(Words.Num());
    Plan.WordPhraseIndices.Reserve(Words.Num());
    Plan.WordSyllableCounts.Reserve(Words.Num());
    Plan.WordBoundaryPunctuationAfter.Reserve(Words.Num());
    for (const FWordSpan& Word : Words)
    {
        Plan.WordSentenceIslandIndices.Add(Word.SentenceIslandIndex);
        Plan.WordPhraseIndices.Add(Word.PhraseIndex);
        Plan.WordSyllableCounts.Add(FMath::Max(1, EstimateSyllableCount(Word.Word)));
        Plan.WordBoundaryPunctuationAfter.Add(Word.BoundaryPunctuationAfter);
    }

    TArray<TArray<FSBSpec>> SpecsByWord;
    SpecsByWord.SetNum(Words.Num());
    for (int32 WordIndex = 0; WordIndex < Words.Num(); ++WordIndex)
    {
        bool bUsedPronunciation = false;
        TArray<FSBSpec> Specs = SB_PronunciationSpecsForWord(Words[WordIndex].Word, bUsedPronunciation);
        if (!bUsedPronunciation || Specs.Num() <= 0)
        {
            Specs = SB_SpecialSpecsForWord(Words[WordIndex].Word);
            if (Specs.Num() <= 0)
            {
                Specs = SB_GenericSpecsForWord(Words[WordIndex].Word);
            }
            SB_ApplyGeneralizedVisibilityRules(Words[WordIndex].Word, Specs);
        }
        SpecsByWord[WordIndex] = Specs;
    }

    const FSBTimingShape TimingShape = SB_AnalyzeTimingShape(Words, &OriginalWords);
    const float EstimatedDuration = SB_EstimateDurationSeconds(Text, Words, &OriginalWords, MinDurationSeconds);

    TArray<float> WordUnits;
    WordUnits.Reserve(Words.Num());
    float TotalUnits = 0.0f;
    int32 LastPhrase = Words[0].PhraseIndex;
    for (int32 I = 0; I < Words.Num(); ++I)
    {
        if (I > 0 && Words[I].PhraseIndex != LastPhrase)
        {
            TotalUnits += 0.12f; // tiny internal text gap; sentence pauses are audio-island anchored
            LastPhrase = Words[I].PhraseIndex;
        }
        float Unit = SB_WordUnit(Words[I].Word) + SB_SpecComplexityUnitBonus(SpecsByWord[I]);
        if (SB_IsCompactContraction(Words[I].Word))
        {
            Unit *= 0.84f;
        }
        const float Position01 = Words.Num() > 1 ? static_cast<float>(I) / static_cast<float>(Words.Num() - 1) : 0.0f;
        if (TimingShape.bLongDeclarative && !SB_IsFunctionOrReducedWord(Words[I].Word))
        {
            if (Position01 >= 0.42f)
            {
                Unit *= 1.0f + 0.10f * ((Position01 - 0.42f) / 0.58f);
            }
            if (EstimateSyllableCount(Words[I].Word) >= 3)
            {
                Unit += 0.06f;
            }
        }
        else if (TimingShape.bLongNarrative && !SB_IsFunctionOrReducedWord(Words[I].Word))
        {
            if (Position01 >= 0.36f)
            {
                Unit *= 1.0f + 0.12f * ((Position01 - 0.36f) / 0.64f);
            }
            if (EstimateSyllableCount(Words[I].Word) >= 3)
            {
                Unit += 0.04f;
            }
        }
        else if (TimingShape.bCadenceList && IsMeaningfulWord(Words[I].Word))
        {
            Unit *= Position01 >= 0.60f ? 1.08f : 1.04f;
        }
        if (TimingShape.bSentenceChain)
        {
            const int32 PhraseIndex = Words[I].PhraseIndex;
            if (PhraseIndex > 0)
            {
                Unit *= 1.08f + 0.03f * FMath::Min(PhraseIndex, 2);
            }
        }
        else if (TimingShape.bCompactConversationalTurn)
        {
            if (Words[I].PhraseIndex > 0)
            {
                Unit *= 1.08f;
            }
        }
        else if (TimingShape.bMultiClauseExplainer)
        {
            if (Words[I].PhraseIndex > 0)
            {
                Unit *= 1.10f + 0.03f * FMath::Min(Words[I].PhraseIndex, 2);
                if (TimingShape.LastPhraseWords <= 7)
                {
                    Unit *= 1.06f;
                }
            }
        }
        if (TimingShape.bProperNounHeavy && OriginalWords.IsValidIndex(I) && IsTitleCaseWord(OriginalWords[I].Word))
        {
            Unit += 0.18f;
            if (Position01 >= 0.45f)
            {
                Unit *= 1.06f;
            }
        }
        WordUnits.Add(Unit);
        TotalUnits += Unit;
    }
    TotalUnits = FMath::Max(TotalUnits, 1.0f);

    TArray<float> WordStartNorm;
    TArray<float> WordEndNorm;
    WordStartNorm.SetNum(Words.Num());
    WordEndNorm.SetNum(Words.Num());
    float Cursor = 0.0f;
    LastPhrase = Words[0].PhraseIndex;
    TMap<int32, int32> PhraseWordCounts;
    TMap<int32, TArray<int32>> PhraseWordIndices;
    for (int32 I = 0; I < Words.Num(); ++I)
    {
        if (I > 0 && Words[I].PhraseIndex != LastPhrase)
        {
            Cursor += 0.12f; // tiny internal text gap; sentence pauses are audio-island anchored
            LastPhrase = Words[I].PhraseIndex;
        }
        WordStartNorm[I] = Cursor / TotalUnits;
        Cursor += WordUnits[I];
        WordEndNorm[I] = Cursor / TotalUnits;
        PhraseWordCounts.FindOrAdd(Words[I].PhraseIndex) += 1;
        PhraseWordIndices.FindOrAdd(Words[I].PhraseIndex).Add(I);
    }

    TMap<int32, float> PhraseStartNorm;
    TMap<int32, float> PhraseEndNorm;
    for (int32 I = 0; I < Words.Num(); ++I)
    {
        if (float* ExistingStart = PhraseStartNorm.Find(Words[I].PhraseIndex))
        {
            *ExistingStart = FMath::Min(*ExistingStart, WordStartNorm[I]);
            PhraseEndNorm[Words[I].PhraseIndex] = FMath::Max(PhraseEndNorm[Words[I].PhraseIndex], WordEndNorm[I]);
        }
        else
        {
            PhraseStartNorm.Add(Words[I].PhraseIndex, WordStartNorm[I]);
            PhraseEndNorm.Add(Words[I].PhraseIndex, WordEndNorm[I]);
        }
    }

    TArray<FSBIntent> Intent;
    for (int32 WordIndex = 0; WordIndex < Words.Num(); ++WordIndex)
    {
        const FWordSpan& Word = Words[WordIndex];
        const float WordStart = WordStartNorm[WordIndex];
        const float WordEnd = WordEndNorm[WordIndex];
        TArray<FSBSpec> Specs = SpecsByWord[WordIndex];

        TArray<TPair<FName, float>> Seen;
        const int32 PhraseCount = PhraseWordCounts.FindRef(Word.PhraseIndex);
        const TArray<int32>& PhraseWords = PhraseWordIndices.FindChecked(Word.PhraseIndex);
        const int32 PhrasePos = PhraseWords.IndexOfByKey(WordIndex);
        const float PhraseSpan = FMath::Max(0.0f, PhraseEndNorm.FindRef(Word.PhraseIndex) - PhraseStartNorm.FindRef(Word.PhraseIndex));

        Specs.Sort([](const FSBSpec& A, const FSBSpec& B)
        {
            return A.Fraction == B.Fraction ? SB_SpecImportanceForCleanup(A) > SB_SpecImportanceForCleanup(B) : A.Fraction < B.Fraction;
        });

        for (const FSBSpec& Spec : Specs)
        {
            const float Center = FMath::Clamp(WordStart + (WordEnd - WordStart) * Spec.Fraction, 0.0f, 1.0f);
            const float Width = FMath::Max(0.010f, (WordEnd - WordStart) * Spec.WidthFraction);
            const bool bTemplateProtectedSupport = Spec.SupportKind == TEXT("template_protected");
            const bool bSibilantPose = Spec.PoseID == TEXT("14_ChJjSh");
            const float SamePoseGap = bSibilantPose
                ? FMath::Max(0.018f, (WordEnd - WordStart) * 0.24f)
                : (bTemplateProtectedSupport
                    ? FMath::Max(0.018f, (WordEnd - WordStart) * 0.22f)
                    : FMath::Max(0.042f, (WordEnd - WordStart) * (SB_IsLandmarkPose(Spec.PoseID) ? 0.58f : 0.46f)));
            bool bDuplicate = false;
            for (const TPair<FName, float>& Prev : Seen)
            {
                if (Prev.Key == Spec.PoseID && FMath::Abs(Prev.Value - Center) < SamePoseGap)
                {
                    bDuplicate = true;
                    break;
                }
            }
            if (bDuplicate) { continue; }
            Seen.Add(TPair<FName, float>(Spec.PoseID, Center));

            FSBIntent E;
            E.PoseID = Spec.PoseID;
            E.CenterNorm = Center;
            E.StartNorm = FMath::Clamp(Center - Width * 0.5f, 0.0f, 1.0f);
            E.EndNorm = FMath::Clamp(Center + Width * 0.5f, 0.0f, 1.0f);
            E.SourceWord = Word.Word;
            E.WordIndex = WordIndex;
            E.PhraseIndex = Word.PhraseIndex;
            E.SentenceIslandIndex = Word.SentenceIslandIndex;
            E.PhraseWordCount = PhraseCount;
            E.PhraseWordFromEnd = FMath::Max(0, PhraseCount - PhrasePos - 1);
            E.PhraseSpanNorm = PhraseSpan;
            E.bIsLandmark = Spec.bHasLandmarkOverride ? Spec.bLandmarkOverride : SB_IsLandmarkPose(Spec.PoseID);
            E.bIsReduced = SB_IsFunctionOrReducedWord(Word.Word);
            E.BaseImportance = Spec.bHasImportanceOverride ? Spec.ImportanceOverride : SB_AnchorImportance(Spec.PoseID);
            E.SupportKind = Spec.SupportKind;
            E.SupportClass = SB_SupportClass(Spec, E.bIsLandmark);
            Intent.Add(E);
        }
    }

    Intent.Sort([](const FSBIntent& A, const FSBIntent& B)
    {
        return A.CenterNorm == B.CenterNorm ? A.BaseImportance > B.BaseImportance : A.CenterNorm < B.CenterNorm;
    });

    for (int32 I = 0; I < Intent.Num(); ++I)
    {
        const float CenterSec = Intent[I].CenterNorm * EstimatedDuration;
        TArray<float> NeighborGaps;
        float Nearest = 0.220f;
        int32 Density = 0;
        float LocalAnchorCompetition = 0.0f;
        float LocalAnchorGap = 0.240f;
        float LocalWordAnchor = 0.0f;
        int32 SameWordCount = 0;

        for (int32 J = 0; J < Intent.Num(); ++J)
        {
            if (Intent[J].WordIndex == Intent[I].WordIndex)
            {
                ++SameWordCount;
            }
            if (I == J) { continue; }
            const float Gap = FMath::Abs(Intent[J].CenterNorm * EstimatedDuration - CenterSec);
            if (Gap <= 0.220f)
            {
                Nearest = FMath::Min(Nearest, Gap);
                if (Gap <= 0.140f) { ++Density; }
            }
            if (Intent[J].bIsLandmark && Gap <= 0.240f)
            {
                if (!Intent[I].bIsLandmark)
                {
                    LocalAnchorCompetition = FMath::Max(LocalAnchorCompetition, Intent[J].BaseImportance);
                    LocalAnchorGap = FMath::Min(LocalAnchorGap, Gap);
                }
                if (Intent[J].WordIndex == Intent[I].WordIndex)
                {
                    LocalWordAnchor = FMath::Max(LocalWordAnchor, Intent[J].BaseImportance);
                }
            }
        }

        const float Pressure = FMath::Clamp((0.155f - Nearest) / 0.120f, 0.0f, 1.0f) * 0.65f
            + FMath::Clamp((static_cast<float>(Density) - 1.0f) / 3.0f, 0.0f, 1.0f) * 0.35f;
        Intent[I].ArticulationPressure = Pressure;

        float AnchorFloor = Intent[I].bIsLandmark ? 0.58f : 0.22f;
        if (Intent[I].PoseID == TEXT("12_Ww-Oo-") || Intent[I].PoseID == TEXT("11_Oo") || Intent[I].PoseID == TEXT("22_MBP"))
        {
            AnchorFloor += 0.06f;
        }
        if (Intent[I].SupportKind == TEXT("front_stop_support"))
        {
            AnchorFloor = 0.16f;
        }
        else if (Intent[I].SupportKind == TEXT("dental_support") || Intent[I].SupportKind == TEXT("final_stop_support"))
        {
            AnchorFloor = 0.12f;
        }
        else if (Intent[I].SupportKind == TEXT("liquid_front_travel"))
        {
            AnchorFloor = 0.12f;
        }

        const float ReducedPenalty = (Intent[I].bIsReduced && !Intent[I].bIsLandmark) ? 0.10f : 0.0f;
        float PhraseEdgeBonus = 0.0f;
        if (Intent[I].WordIndex == 0 || I == Intent.Num() - 1)
        {
            if (Intent[I].bIsLandmark)
            {
                PhraseEdgeBonus = 0.08f;
            }
            else if (Intent[I].SupportClass == TEXT("open_support") && SameWordCount <= 4)
            {
                PhraseEdgeBonus = 0.03f;
            }
        }

        float Realization = Intent[I].BaseImportance
            - Pressure * SB_ClassPressureLoss(Intent[I].SupportClass)
            - ReducedPenalty
            + PhraseEdgeBonus;

        if (Intent[I].SupportKind == TEXT("template_protected"))
        {
            Realization += 0.08f * FMath::Max(0.0f, 1.0f - Pressure * 0.50f);
        }
        else if (Intent[I].SupportKind == TEXT("sibilant_onset"))
        {
            Realization += 0.03f * FMath::Max(0.0f, 1.0f - Pressure * 0.60f);
        }
        else if (Intent[I].SupportKind == TEXT("final_sibilant_support"))
        {
            Realization += 0.02f * FMath::Max(0.0f, 1.0f - Pressure * 0.70f);
        }
        else if (Intent[I].SupportKind == TEXT("internal_sibilant_support"))
        {
            Realization += 0.02f * FMath::Max(0.0f, 1.0f - Pressure * 0.70f);
        }
        else if (Intent[I].SupportKind == TEXT("round_glide_support"))
        {
            Realization += 0.02f * FMath::Max(0.0f, 1.0f - Pressure * 0.70f);
        }
        else if (Intent[I].SupportKind == TEXT("round_syllable_support"))
        {
            Realization += 0.04f * FMath::Max(0.0f, 1.0f - Pressure * 0.55f);
        }

        if (Intent[I].SupportKind == TEXT("front_stop_support"))
        {
            Realization -= 0.05f + 0.04f * Pressure;
        }
        else if (Intent[I].SupportKind == TEXT("sibilant_onset"))
        {
            Realization -= 0.01f + 0.03f * Pressure;
        }
        else if (Intent[I].SupportKind == TEXT("final_sibilant_support"))
        {
            Realization -= 0.04f + 0.04f * Pressure;
        }
        else if (Intent[I].SupportKind == TEXT("internal_sibilant_support"))
        {
            Realization -= 0.05f + 0.05f * Pressure;
        }
        else if (Intent[I].SupportKind == TEXT("round_glide_support"))
        {
            Realization -= 0.03f + 0.05f * Pressure;
        }
        else if (Intent[I].SupportKind == TEXT("round_syllable_support"))
        {
            Realization -= 0.01f + 0.03f * Pressure;
        }
        else if (Intent[I].SupportKind == TEXT("rhotic_support"))
        {
            Realization -= 0.04f + 0.04f * Pressure;
        }
        else if (Intent[I].SupportKind == TEXT("dental_support"))
        {
            Realization -= 0.10f + 0.08f * Pressure;
        }
        else if (Intent[I].SupportKind == TEXT("final_stop_support"))
        {
            Realization -= 0.14f + 0.08f * Pressure;
        }
        else if (Intent[I].SupportKind == TEXT("medial_vowel_support"))
        {
            Realization -= 0.08f + 0.08f * Pressure;
        }
        else if (Intent[I].SupportKind == TEXT("terminal_reduction_support"))
        {
            Realization -= 0.12f + 0.08f * Pressure;
        }
        else if (Intent[I].SupportKind == TEXT("liquid_front_travel"))
        {
            Realization -= 0.12f + 0.08f * Pressure;
        }
        else if (Intent[I].SupportClass == TEXT("front_vowel_support"))
        {
            Realization -= 0.04f * Pressure;
        }
        else if (Intent[I].SupportClass == TEXT("relaxed_round_support"))
        {
            Realization -= 0.05f + 0.06f * Pressure;
        }
        else if (Intent[I].SupportClass == TEXT("open_support") && LocalWordAnchor >= 0.84f)
        {
            Realization -= 0.05f * FMath::Max(0.0f, 1.0f - LocalAnchorGap / 0.140f);
        }

        if (SameWordCount >= 3 && LocalWordAnchor >= 0.84f && (Intent[I].SupportClass == TEXT("front_vowel_support") || Intent[I].SupportClass == TEXT("round_support")))
        {
            Realization += 0.03f * FMath::Max(0.0f, 1.0f - Pressure * 0.70f);
        }

        if (Pressure < 0.25f && !Intent[I].bIsLandmark && Intent[I].SupportKind != TEXT("liquid_front_travel"))
        {
            float SparseBonus = 0.08f;
            if (Intent[I].SupportKind == TEXT("front_stop_support"))
            {
                SparseBonus *= 0.35f;
            }
            if (LocalAnchorCompetition >= 0.84f && LocalAnchorGap <= 0.135f)
            {
                SparseBonus *= (Intent[I].SupportClass == TEXT("front_vowel_support") || Intent[I].SupportClass == TEXT("relaxed_round_support") || Intent[I].SupportClass == TEXT("round_support")) ? 0.25f : 0.0f;
            }
            Realization += SparseBonus;
        }

        const float PhraseSec = Intent[I].PhraseSpanNorm * EstimatedDuration;
        const float DurationCompact = FMath::Clamp((1.20f - PhraseSec) / 0.55f, 0.0f, 1.0f);
        const float WordCompact = FMath::Clamp((5.0f - static_cast<float>(Intent[I].PhraseWordCount)) / 4.0f, 0.0f, 1.0f);
        const float CompactTail = DurationCompact * FMath::Max(0.55f, WordCompact);
        if (!Intent[I].bIsLandmark && CompactTail > 0.0f && Intent[I].PhraseWordCount <= 4)
        {
            const float TailWeight = Intent[I].PhraseWordFromEnd > 1 ? 0.75f : 1.0f;
            float PhraseAnchorCompetition = 0.0f;
            for (int32 J = 0; J < Intent.Num(); ++J)
            {
                if (I == J || Intent[J].PhraseIndex != Intent[I].PhraseIndex || !Intent[J].bIsLandmark) { continue; }
                const float Gap = FMath::Abs(Intent[J].CenterNorm * EstimatedDuration - CenterSec);
                if (Gap <= 0.240f)
                {
                    PhraseAnchorCompetition = FMath::Max(PhraseAnchorCompetition, Intent[J].BaseImportance);
                }
            }
            float Ceiling = 0.80f - 0.12f * CompactTail * TailWeight - 0.10f * Pressure;
            if (Intent[I].PhraseWordFromEnd <= 1) { Ceiling -= 0.05f * CompactTail; }
            if (PhraseAnchorCompetition >= 0.84f) { Ceiling -= 0.05f * CompactTail; }
            if (Intent[I].PoseID == TEXT("07_Aa") || Intent[I].PoseID == TEXT("09_Oh") || Intent[I].PoseID == TEXT("10_Or") || Intent[I].PoseID == TEXT("11_Oo"))
            {
                Ceiling -= 0.04f * CompactTail;
            }
            Realization = FMath::Min(Realization, FMath::Max(0.30f, Ceiling));
        }

        if (!Intent[I].bIsLandmark && Intent[I].SupportKind != TEXT("template_protected") && LocalAnchorCompetition >= 0.84f && LocalAnchorGap <= 0.145f)
        {
            if (Intent[I].SupportClass == TEXT("liquid_front_travel"))
            {
                Realization = FMath::Min(Realization, Pressure > 0.40f ? 0.16f : 0.20f);
            }
            else if (Intent[I].SupportClass == TEXT("medial_vowel_support"))
            {
                Realization = FMath::Min(Realization, Pressure > 0.40f ? 0.48f : 0.54f);
            }
            else if (Intent[I].SupportClass == TEXT("front_stop_support"))
            {
                Realization = FMath::Min(Realization, Pressure > 0.40f ? 0.24f : 0.28f);
            }
            else if (Intent[I].SupportClass == TEXT("front_vowel_support"))
            {
                Realization = FMath::Min(Realization, Pressure > 0.55f ? 0.32f : 0.38f);
            }
            else if (Intent[I].SupportClass == TEXT("relaxed_round_support"))
            {
                Realization = FMath::Min(Realization, Pressure > 0.40f ? 0.30f : 0.36f);
            }
            else if (Intent[I].SupportClass == TEXT("round_support"))
            {
                Realization = FMath::Min(Realization, Pressure > 0.40f ? 0.36f : 0.42f);
            }
            else if (Intent[I].SupportClass == TEXT("open_support") && SameWordCount >= 3)
            {
                Realization = FMath::Min(Realization, Pressure > 0.40f ? 0.40f : 0.48f);
            }
        }

        float Realized = FMath::Clamp(Realization, AnchorFloor, 1.0f);
        if (Intent[I].SupportKind == TEXT("template_protected"))
        {
            Realized = FMath::Max(Realized, FMath::Min(0.78f, Intent[I].BaseImportance));
        }
        else if (Intent[I].SupportKind == TEXT("sibilant_onset"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.50f : 0.54f);
        }
        else if (Intent[I].SupportKind == TEXT("final_sibilant_support"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.44f : 0.48f);
        }
        else if (Intent[I].SupportKind == TEXT("internal_sibilant_support"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.46f : 0.50f);
        }
        else if (Intent[I].SupportKind == TEXT("round_glide_support"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.50f : 0.54f);
        }
        else if (Intent[I].SupportKind == TEXT("round_syllable_support"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.58f : 0.64f);
        }
        else if (Intent[I].SupportKind == TEXT("rhotic_support"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.52f : 0.58f);
        }
        if (Intent[I].SupportKind == TEXT("medial_vowel_support"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.50f : 0.54f);
        }
        else if (Intent[I].SupportKind == TEXT("dental_support"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.30f : 0.34f);
        }
        else if (Intent[I].SupportKind == TEXT("final_stop_support"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.18f : 0.22f);
        }
        else if (Intent[I].SupportKind == TEXT("terminal_reduction_support"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.42f : 0.46f);
        }
        else if (Intent[I].SupportKind == TEXT("front_stop_support"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.28f : 0.32f);
        }
        else if (Intent[I].SupportKind == TEXT("liquid_front_travel"))
        {
            Realized = FMath::Min(Realized, Pressure > 0.45f ? 0.16f : 0.20f);
        }
        Intent[I].Realization = FMath::Clamp(Realized, 0.0f, 1.0f);
    }

    for (const FSBIntent& E : Intent)
    {
        FOffgridAITextVisemeEvent Event;
        Event.StartNorm = FMath::Clamp(E.StartNorm, 0.0f, 1.0f);
        Event.EndNorm = FMath::Clamp(E.EndNorm, Event.StartNorm, 1.0f);
        Event.PoseID = E.PoseID;
        Event.Viseme = LegacyVisemeForPose(E.PoseID);
        Event.Strength = FMath::Clamp(E.Realization, 0.0f, 1.0f);
        Event.SourceText = E.SourceWord;
        Event.WordIndex = E.WordIndex;
        Event.PhraseIndex = E.PhraseIndex;
        Event.SentenceIslandIndex = E.SentenceIslandIndex;
        Event.bIsLandmark = E.bIsLandmark;
        Event.bIsDominant = E.bIsLandmark || Event.Strength >= 0.78f;
        Event.bIsFunctionWord = E.bIsReduced;
        Event.Generator = FName(TEXT("SimpleBudgetV1"));
        if (Event.PoseID != NAME_None && Event.Viseme != EOffgridAITextViseme::Rest && Event.Strength > 0.0f)
        {
            Plan.Events.Add(Event);
        }
    }

    Plan.EstimatedDurationSeconds = SB_RefineEstimatedDurationWithEvents(Plan, EstimatedDuration);
    Plan.Layer1Diagnostics.StageCounts.CandidateCount = Plan.Events.Num();
    Plan.Layer1Diagnostics.StageCounts.TimedCandidateCount = Plan.Events.Num();
    Plan.Layer1Diagnostics.StageCounts.FinalEventCount = Plan.Events.Num();
    Plan.Layer1Diagnostics.SuppressionCount = 0;
    Plan.Layer1Diagnostics.PhraseFinalAdjustmentCount = 0;
    Plan.Layer1Diagnostics.CompressionRatio = 1.0f;
    return Plan;
}

const TCHAR* FOffgridAITextVisemePlanner::ToPoseKey(EOffgridAITextViseme Viseme)
{
    switch (Viseme)
    {
    case EOffgridAITextViseme::MBP: return TEXT("MBP");
    case EOffgridAITextViseme::AAA: return TEXT("AAA");
    case EOffgridAITextViseme::EEE: return TEXT("EEE");
    case EOffgridAITextViseme::OOO: return TEXT("OOO");
    case EOffgridAITextViseme::WUH: return TEXT("WUH");
    case EOffgridAITextViseme::FVS: return TEXT("FVS");
    default: return TEXT("REST");
    }
}

FString FOffgridAITextVisemePlanner::ToDebugString(EOffgridAITextViseme Viseme)
{
    return FString(FOffgridAITextVisemePlanner::ToPoseKey(Viseme));
}
