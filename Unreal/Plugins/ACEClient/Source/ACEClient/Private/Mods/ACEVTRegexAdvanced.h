#pragma once
#include "CoreMinimal.h"
#include "ACEVTRegex.h"

// .NET capture-history constructs that ICU cannot represent. Input is the
// normalized pattern from ACEVTRegex; ordinary expressions retain the ICU path.
namespace ACEVTRegexAdvanced
{
struct FProgram;
bool Required(const FString& Pattern);
TSharedPtr<const FProgram> Compile(const FString& Pattern, FString& Error);
bool Match(const FProgram& Program, const FString& Text, ACEVTRegex::FCaptures* Captures, FString& Error);
}
