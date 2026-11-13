#include "ACEVTRegex.h"
#include "ACEVTRegexAdvanced.h"
#include "ACEVTRegexKeys.h"
#include "Containers/StringConv.h"
#include "Containers/LruCache.h"
#if UE_ENABLE_ICU
THIRD_PARTY_INCLUDES_START
#include <unicode/uregex.h>
THIRD_PARTY_INCLUDES_END
#endif

namespace ACEVTRegex
{
#if UE_ENABLE_ICU
// Validation is a property of the exact pattern, not the current profile or
// whether looting is enabled. Keep only diagnostics here, not ICU matchers:
// large loot profiles have more unique patterns than the hot matcher cache.
static thread_local TLruCache<FString,FString,FACERegexExactComparer> ValidationCache(4096);
#if WITH_DEV_AUTOMATION_TESTS
static thread_local uint64 ValidationCompilations=0;
#endif
// Strip .NET x/n options lexically instead of delegating x to ICU: its
// whitespace/comment rules differ, and captures must retain .NET numbering.
static bool Normalize(const FString& Input,FString& Pattern,FString& Error)
{
    struct FOptions {bool Extended=false,Explicit=false;};
    FOptions Options;TArray<FOptions> Stack;bool InClass=false;int ClassStart=-1;
    for(int At=0;At<Input.Len();++At)
    {
        const TCHAR C=Input[At];
        if(!InClass&&Options.Extended)
        {
            if(C==' '||C=='\t'||C=='\r'||C=='\n'||C=='\f')continue;
            if(C=='#'){while(At+1<Input.Len()&&Input[At+1]!='\n')++At;continue;}
        }
        if(!InClass&&(Input.Mid(At,3)==TEXT("\\k'")||Input.Mid(At,3)==TEXT("\\k<")))
        {
            const int End=Input.Find(Input[At+2]=='<'?TEXT(">"):TEXT("'"),ESearchCase::CaseSensitive,ESearchDir::FromStart,At+3);
            if(End==INDEX_NONE){Error=TEXT("Unterminated named backreference");return false;}
            Pattern+=TEXT("\\k<")+Input.Mid(At+3,End-At-3)+TEXT(">");At=End;continue;
        }
        if(C=='\\')
        {
            // Escape arguments are tokens, not x-mode whitespace/comments.
            // In particular keep Unicode-property braces out of the literal
            // brace normalization below, and preserve .NET's 2–4 digit \x.
            if(At+2<Input.Len()&&(Input[At+1]=='p'||Input[At+1]=='P')&&Input[At+2]=='{')
            {
                const int End=Input.Find(TEXT("}"),ESearchCase::CaseSensitive,ESearchDir::FromStart,At+3);
                if(End==INDEX_NONE){Error=TEXT("Unterminated Unicode property escape");return false;}
                Pattern+=Input.Mid(At,End-At+1);At=End;continue;
            }
            if(At+1<Input.Len()&&(Input[At+1]=='u'||Input[At+1]=='x'))
            {
                int End=At+2;uint32 Value=0;
                while(End<Input.Len()&&End<At+6&&FChar::IsHexDigit(Input[End])){Value=Value*16+FParse::HexDigit(Input[End]);++End;}
                if(End-At-2<(Input[At+1]=='u'?4:2)){Error=TEXT("Invalid hexadecimal regex escape");return false;}
                Pattern+=FString::Printf(TEXT("\\x{%X}"),Value);At=End-1;continue;
            }
            Pattern+=C;if(At+1<Input.Len())Pattern+=Input[++At];continue;
        }
        if(InClass)
        {
            Pattern+=C;
            if(C==']'&&At>ClassStart+1&&!(At==ClassStart+2&&Input[ClassStart+1]=='^'))InClass=false;
            continue;
        }
        if(C=='['){InClass=true;ClassStart=At;Pattern+=C;continue;}
        if(C=='{')
        {
            // .NET recognizes quantifiers before ignoring x-mode whitespace.
            // A spaced brace expression stays literal; stripping it first
            // would silently turn e.g. a{1, 2} into a repetition.
            int End=At+1;while(End<Input.Len()&&Input[End]>='0'&&Input[End]<='9')++End;
            const bool Digits=End>At+1;
            if(End<Input.Len()&&Input[End]==','){++End;while(End<Input.Len()&&Input[End]>='0'&&Input[End]<='9')++End;}
            if(Digits&&End<Input.Len()&&Input[End]=='}'){Pattern+=Input.Mid(At,End-At+1);At=End;}
            else Pattern+=TEXT("\\{");
            continue;
        }
        if(C=='}'){Pattern+=TEXT("\\}");continue;}
        if(!InClass&&Input.Mid(At,3)==TEXT("(?#"))
        {
            const int End=Input.Find(TEXT(")"),ESearchCase::CaseSensitive,ESearchDir::FromStart,At+3);
            if(End==INDEX_NONE){Error=TEXT("Unterminated regex comment");return false;}
            At=End;continue;
        }
        if(Input.Mid(At,3)==TEXT("(?("))
        {
            // The condition's opening parenthesis is syntax, not an unnamed
            // capture. Preserve it under explicit-capture (n) mode too.
            Stack.Add(Options);Stack.Add(Options);Pattern+=TEXT("(?(");At+=2;continue;
        }
        if(Input.Mid(At,2)==TEXT("(?"))
        {
            if(At+2<Input.Len()&&FChar::IsWhitespace(Input[At+2])){Error=TEXT("Whitespace in regex group prefix");return false;}
            int End=At+2;bool Disable=false,Any=false;FOptions Changed=Options;FString On,Off;
            for(;End<Input.Len();++End)
            {
                const TCHAR Flag=Input[End];
                if(Flag=='-'){Disable=true;continue;}
                if(Flag!='i'&&Flag!='m'&&Flag!='s'&&Flag!='x'&&Flag!='n')break;
                Any=true;
                if(Flag=='x')Changed.Extended=!Disable;
                else if(Flag=='n')Changed.Explicit=!Disable;
                else (Disable?Off:On)+=Flag;
            }
            if(Any&&End<Input.Len()&&(Input[End]==':'||Input[End]==')'))
            {
                const bool Scoped=Input[End]==':';if(Scoped)Stack.Add(Options);Options=Changed;
                const FString Flags=On+(Off.IsEmpty()?FString():TEXT("-")+Off);
                if(!Flags.IsEmpty())Pattern+=TEXT("(?")+Flags+(Scoped?TEXT(":"):TEXT(")"));
                else if(Scoped)Pattern+=TEXT("(?:");
                At=End;continue;
            }
            if(Any){Error=TEXT("Invalid regex option group");return false;}
        }
        if(C=='(')Stack.Add(Options);
        else if(C==')'&&!Stack.IsEmpty())Options=Stack.Pop(EAllowShrinking::No);
        if(Input.Mid(At,3)==TEXT("(?'")||(Input.Mid(At,3)==TEXT("(?<")&&At+3<Input.Len()&&Input[At+3]!='='&&Input[At+3]!='!'))
        {
            const int End=Input.Find(Input[At+2]=='<'?TEXT(">"):TEXT("'"),ESearchCase::CaseSensitive,ESearchDir::FromStart,At+3);
            if(End==INDEX_NONE){Error=TEXT("Unterminated named capture");return false;}
            Pattern+=TEXT("(?<")+Input.Mid(At+3,End-At-3)+TEXT(">");At=End;continue;
        }
        if(C=='('&&Options.Explicit&&(At+1==Input.Len()||Input[At+1]!='?'))Pattern+=TEXT("(?:");
        else Pattern+=C;
    }
    return true;
}
static void CaptureOrder(const FString& Pattern,TArray<int>& Order,TMap<int,FString>& Names)
{
    TArray<int> Named;int Source=0;bool InClass=false;int ClassStart=-1;Order={0};Names.Empty();
    for(int At=0;At<Pattern.Len();++At)
    {
        const TCHAR C=Pattern[At];if(C=='\\'){++At;continue;}
        if(InClass){if(C==']'&&At>ClassStart+1&&!(At==ClassStart+2&&Pattern[ClassStart+1]=='^'))InClass=false;continue;}
        if(C=='['){InClass=true;ClassStart=At;continue;}
        if(C!='(')continue;
        if(At+1==Pattern.Len()||Pattern[At+1]!='?')Order.Add(++Source);
        else if(Pattern.Mid(At,3)==TEXT("(?<")&&At+3<Pattern.Len()&&Pattern[At+3]!='='&&Pattern[At+3]!='!')
        {
            const int End=Pattern.Find(TEXT(">"),ESearchCase::CaseSensitive,ESearchDir::FromStart,At+3);
            if(End!=INDEX_NONE){Named.Add(++Source);Names.Add(Source,Pattern.Mid(At+3,End-At-3));}
        }
    }
    Order.Append(Named);
}
static URegularExpression* Compile(const FString& Input,FString& Error,FString* Normalized=nullptr,TSharedPtr<const ACEVTRegexAdvanced::FProgram>* Advanced=nullptr)
{
    if(Input.Len()>2048){Error=TEXT("Pattern exceeds 2048 characters");return nullptr;}
    FString Pattern;if(!Normalize(Input,Pattern,Error))return nullptr;
    if(ACEVTRegexAdvanced::Required(Pattern))
    {
        if(Normalized)*Normalized=Pattern;
        auto Program=ACEVTRegexAdvanced::Compile(Pattern,Error);
        if(Advanced)*Advanced=MoveTemp(Program);
        return nullptr;
    }
    TArray<int> Order;TMap<int,FString> Names;CaptureOrder(Pattern,Order,Names);
    // Rewrite references using .NET's unnamed-first numbering. Keep the
    // normalized source for capture extraction; no additional groups are added.
    if(Normalized)*Normalized=Pattern;
    FString Rewritten;bool InClass=false;int ClassStart=-1;
    for(int At=0;At<Pattern.Len();++At)
    {
        const TCHAR C=Pattern[At];
        if(C=='\\'&&At+1<Pattern.Len())
        {
            int Start=At+1,End=Start;bool Bracket=false;
            if(!InClass&&Pattern.Mid(At,3)==TEXT("\\k<")){Start=At+3;End=Start;Bracket=true;}
            if(!InClass&&End<Pattern.Len()&&Pattern[End]>='1'&&Pattern[End]<='9')
            {
                while(End<Pattern.Len()&&Pattern[End]>='0'&&Pattern[End]<='9')++End;
                const FString Digits=Pattern.Mid(Start,End-Start);
                const int Number=Digits.Len()<=3?FCString::Atoi(*Digits):MAX_int32;
                if(Bracket&&(End>=Pattern.Len()||Pattern[End]!='>')){Error=TEXT("Invalid numeric backreference");return nullptr;}
                if(Order.IsValidIndex(Number))
                {Rewritten+=TEXT("\\")+FString::FromInt(Order[Number])+TEXT("(?:)");At=End-1+(Bracket?1:0);continue;}
                if(Bracket||Digits.Len()==1||Digits[0]>'7'){Error=TEXT("Undefined numeric backreference");return nullptr;}
                // .NET falls back to at most three octal digits for a missing
                // multi-digit group. ICU would instead try shorter group IDs.
                int Octal=0,Count=0;while(Count<3&&Count<Digits.Len()&&Digits[Count]<='7'){Octal=Octal*8+Digits[Count++]-'0';}
                Rewritten+=FString::Printf(TEXT("\\x{%X}"),Octal&255);At=Start+Count-1;continue;
            }
            Rewritten+=C;Rewritten+=Pattern[++At];continue;
        }
        if(InClass){if(C==']'&&At>ClassStart+1&&!(At==ClassStart+2&&Pattern[ClassStart+1]=='^'))InClass=false;}
        else if(C=='['){InClass=true;ClassStart=At;}
        Rewritten+=C;
    }
    Pattern=MoveTemp(Rewritten);
    if(Pattern.Len()>2048){Error=TEXT("Pattern exceeds 2048 characters");return nullptr;}
    const FString Nonempty=Pattern.IsEmpty()?TEXT("(?:)"):Pattern;
    const auto Utf16=StringCast<UTF16CHAR>(*Nonempty,Nonempty.Len());UErrorCode Status=U_ZERO_ERROR;UParseError Parse{};
    auto* Regex=uregex_open(reinterpret_cast<const UChar*>(Utf16.Get()),Utf16.Length(),0,&Parse,&Status);
    if(U_FAILURE(Status)){Error=FString::Printf(TEXT("Invalid pattern at %d: %s"),Parse.offset,UTF8_TO_TCHAR(u_errorName(Status)));return nullptr;}
    if(uregex_groupCount(Regex,&Status)>32){uregex_close(Regex);Error=TEXT("Pattern exceeds 32 capture groups");return nullptr;}
    uregex_setTimeLimit(Regex,2,&Status);uregex_setStackLimit(Regex,65536,&Status);
    if(U_FAILURE(Status)){uregex_close(Regex);Error=TEXT("Cannot bound regex execution");return nullptr;}
    return Regex;
}
struct FCompiledPattern
{
    URegularExpression* Regex = nullptr;
    TSharedPtr<const ACEVTRegexAdvanced::FProgram> Advanced;
    FString Normalized;
    uint64 Used = 0;
    ~FCompiledPattern() { if (Regex) uregex_close(Regex); }
};
static URegularExpression* CachedClone(const FString& Pattern,FString& Error,FString* Normalized=nullptr,TSharedPtr<const ACEVTRegexAdvanced::FProgram>* Advanced=nullptr)
{
    // Cache text-free templates. Every invocation gets its own matcher, so ICU
    // never retains a pointer to a previous call's temporary UTF-16 input.
    static thread_local TMap<FString,TSharedPtr<FCompiledPattern>,FDefaultSetAllocator,TACERegexExactKeys<TSharedPtr<FCompiledPattern>>> Cache;
    static thread_local uint64 Clock=0;
    auto Entry=Cache.FindRef(Pattern);
    if(!Entry)
    {
        Entry=MakeShared<FCompiledPattern>();
        Entry->Regex=Compile(Pattern,Error,&Entry->Normalized,&Entry->Advanced);if(!Entry->Regex&&!Entry->Advanced)return nullptr;
        if(Cache.Num()>=256)
        {
            FString Oldest;uint64 Age=MAX_uint64;
            for(const auto& Pair:Cache)if(Pair.Value->Used<Age){Age=Pair.Value->Used;Oldest=Pair.Key;}
            Cache.Remove(Oldest);
        }
        Cache.Add(Pattern,Entry);
    }
    Entry->Used=++Clock;if(Normalized)*Normalized=Entry->Normalized;
    if(Entry->Advanced){if(Advanced)*Advanced=Entry->Advanced;return nullptr;}
    UErrorCode Status=U_ZERO_ERROR;
    auto* Regex=uregex_clone(Entry->Regex,&Status);
    if(U_FAILURE(Status)){Error=TEXT("Cannot clone bounded regex");return nullptr;}
    // ICU clones the pattern/flags, not matcher limits.
    uregex_setTimeLimit(Regex,2,&Status);uregex_setStackLimit(Regex,65536,&Status);
    if(U_FAILURE(Status)){uregex_close(Regex);Error=TEXT("Cannot bound cloned regex execution");return nullptr;}
    return Regex;
}
#endif
bool Test(const FString& Pattern,const FString& Text,FString& Error)
{
    Error.Empty();
    if(Text.Len()>4096||Pattern.Len()>8192){Error=TEXT("Regex input exceeds limit");return false;}
#if UE_ENABLE_ICU
    static thread_local TMap<FString,bool,FDefaultSetAllocator,TACERegexExactKeys<bool>> Results;
    const FString Key=FString::FromInt(Pattern.Len())+TEXT(":")+Pattern+Text;
    if(const bool* Found=Results.Find(Key))return *Found;
    TSharedPtr<const ACEVTRegexAdvanced::FProgram> Advanced;
    auto* Regex=CachedClone(Pattern,Error,nullptr,&Advanced);
    if(Advanced)
    {
        const bool Found=ACEVTRegexAdvanced::Match(*Advanced,Text,nullptr,Error);
        if(Error.IsEmpty()){if(Results.Num()>=512)Results.Empty();Results.Add(Key,Found);}return Found;
    }
    if(!Regex)return false;
    const auto Utf16=StringCast<UTF16CHAR>(*Text,Text.Len());UErrorCode Status=U_ZERO_ERROR;
    uregex_setText(Regex,reinterpret_cast<const UChar*>(Utf16.Get()),Utf16.Length(),&Status);
    const bool Found=uregex_find(Regex,0,&Status)!=0;
    uregex_close(Regex);
    if(U_FAILURE(Status)){Error=FString::Printf(TEXT("Regex failed within its work limit: %s"),UTF8_TO_TCHAR(u_errorName(Status)));return false;}
    if(Results.Num()>=512)Results.Empty();
    Results.Add(Key,Found);return Found;
#else
    Error=TEXT("This build lacks ICU regex support");return false;
#endif
}
bool Validate(const FString& Pattern,FString& Error)
{
    Error.Empty();
#if UE_ENABLE_ICU
    if(const auto* Cached=ValidationCache.FindAndTouch(Pattern))
    {Error=*Cached;return Error.IsEmpty();}
#if WITH_DEV_AUTOMATION_TESTS
    ++ValidationCompilations;
#endif
    TSharedPtr<const ACEVTRegexAdvanced::FProgram> Advanced;
    auto* Regex=Compile(Pattern,Error,nullptr,&Advanced);
    const bool Valid=Regex!=nullptr||Advanced.IsValid();
    if(Regex)uregex_close(Regex);
    // Bound memory as well as entry count. Oversized input still goes through
    // the original validator, but cannot occupy a long-lived cache entry.
    if(Pattern.Len()<=2048)ValidationCache.Add(Pattern,Error);
    return Valid;
#else
    Error=TEXT("This build lacks ICU regex support");
    return false;
#endif
}
#if WITH_DEV_AUTOMATION_TESTS
uint64 ValidationCompilationsForTests()
{
#if UE_ENABLE_ICU
    return ValidationCompilations;
#else
    return 0;
#endif
}
int32 ValidationCacheEntriesForTests()
{
#if UE_ENABLE_ICU
    return ValidationCache.Num();
#else
    return 0;
#endif
}
#endif
bool Match(const FString& Pattern,const FString& Text,ACEVTRegex::FCaptures& Captures,FString& Error)
{
    Captures.Empty();Error.Empty();if(Text.Len()>4096){Error=TEXT("Regex input exceeds 4096 characters");return false;}
#if UE_ENABLE_ICU
    TSharedPtr<const ACEVTRegexAdvanced::FProgram> Advanced;
    FString Normalized;auto* Regex=CachedClone(Pattern,Error,&Normalized,&Advanced);
    if(Advanced)return ACEVTRegexAdvanced::Match(*Advanced,Text,&Captures,Error);
    if(!Regex)return false;
    const auto Utf16=StringCast<UTF16CHAR>(*Text,Text.Len());UErrorCode Status=U_ZERO_ERROR;
    uregex_setText(Regex,reinterpret_cast<const UChar*>(Utf16.Get()),Utf16.Length(),&Status);
    const bool Found=uregex_find(Regex,0,&Status)!=0;
    if(Found&&U_SUCCESS(Status))
    {
        const int Count=uregex_groupCount(Regex,&Status);
        if(Count>32)Status=U_REGEX_STACK_OVERFLOW;
        TArray<int> Order;TMap<int,FString> Names;CaptureOrder(Normalized,Order,Names);
        if(Order.Num()!=Count+1)Status=U_UNSUPPORTED_ERROR;
        if(U_SUCCESS(Status))for(int I=0;I<=Count;++I)
        {
            const int Group=Order[I],Start=uregex_start(Regex,Group,&Status),End=uregex_end(Regex,Group,&Status);
            if(Start>=0&&End>=Start){const auto Value=StringCast<TCHAR>(Utf16.Get()+Start,End-Start);const FString Result(Value.Length(),Value.Get());Captures.Add(FString::FromInt(I),Result);if(const auto* Name=Names.Find(Group))Captures.Add(*Name,Result);}
        }
    }
    uregex_close(Regex);
    if(U_FAILURE(Status)){Error=FString::Printf(TEXT("Regex failed within its work limit: %s"),UTF8_TO_TCHAR(u_errorName(Status)));Captures.Empty();return false;}
    return Found;
#else
    Error=TEXT("This build lacks ICU regex support");return false;
#endif
}
}
