#pragma once
#include "CoreMinimal.h"
#include "Misc/Crc.h"

// FString's default key equality ignores case. Regex source, input, and group
// names require ordinal keys even when a pattern opts into case folding.
struct FACERegexExactComparer
{
    static bool Matches(const FString& A,const FString& B){return A.Equals(B,ESearchCase::CaseSensitive);}
    static uint32 GetKeyHash(const FString& Key){return FCrc::StrCrc32(*Key);}
};
template<typename Value>
struct TACERegexExactKeys:TDefaultMapHashableKeyFuncs<FString,Value,false>
{
    static bool Matches(const FString& A,const FString& B){return FACERegexExactComparer::Matches(A,B);}
    static uint32 GetKeyHash(const FString& Key){return FACERegexExactComparer::GetKeyHash(Key);}
};
