#pragma once
#include "CoreMinimal.h"
#include "ACEVTRegexKeys.h"

// ICU matching with explicit work/stack limits. Never run an unrestricted
// backtracking engine on imported profiles on the game thread.
namespace ACEVTRegex
{
using FCaptures=TMap<FString,FString,FDefaultSetAllocator,TACERegexExactKeys<FString>>;
// Exact-pattern validation (including diagnostics) uses a bounded cache;
// changes to pattern text always go through validation again.
bool Validate(const FString& Pattern, FString& Error);
#if WITH_DEV_AUTOMATION_TESTS
uint64 ValidationCompilationsForTests();
int32 ValidationCacheEntriesForTests();
#endif
// Boolean filters do not allocate capture maps. Compiled patterns and exact
// predicate results use bounded, thread-local caches; failures are never cached.
bool Test(const FString& Pattern, const FString& Text, FString& Error);
bool Match(const FString& Pattern, const FString& Text, FCaptures& Captures, FString& Error);
}
