#include "ACEVTRegexAdvanced.h"
#include "ACEVTRegexKeys.h"
#include "Containers/StringConv.h"
#include "HAL/PlatformTime.h"
#if UE_ENABLE_ICU
THIRD_PARTY_INCLUDES_START
#include <unicode/uregex.h>
#include <unicode/ustring.h>
THIRD_PARTY_INCLUDES_END
#endif

namespace ACEVTRegexAdvanced
{
bool Required(const FString& Pattern)
{
    bool InClass=false;TArray<FString> Names;
    for(int I=0;I<Pattern.Len();++I)
    {
        if(Pattern[I]=='\\'){++I;continue;}
        if(Pattern[I]=='[')InClass=true;
        if(Pattern[I]==']')InClass=false;
        if(InClass)continue;
        if(Pattern.Mid(I,3)==TEXT("(?("))return true;
        if(Pattern.Mid(I,3)==TEXT("(?<")&&I+3<Pattern.Len()&&Pattern[I+3]!='='&&Pattern[I+3]!='!')
        {
            int End=Pattern.Find(TEXT(">"),ESearchCase::CaseSensitive,ESearchDir::FromStart,I+3);
            if(End!=INDEX_NONE)
            {
                const FString Name=Pattern.Mid(I+3,End-I-3);
                if(Name.Contains(TEXT("-"))||(!Name.IsEmpty()&&FChar::IsDigit(Name[0])))return true;
                for(const FString& Prior:Names)if(Prior.Equals(Name,ESearchCase::CaseSensitive))return true;
                Names.Add(Name);I=End;
            }
        }
    }
    return false;
}
#if UE_ENABLE_ICU
namespace
{
constexpr int MaxGroups=32, MaxLoops=32, MaxDepth=64, MaxBytes=65536;
enum class ENode:uint8 { Empty, Atom, Sequence, Alternative, Capture, Balance, Backref, Conditional, Look, Atomic, Repeat, SearchStart };
struct FNode
{
    ENode Kind=ENode::Empty;
    TArray<int> Children;
    FString Name, Pop, Text;
    int Group=-1, Subtract=-1, Min=0, Max=0, Ordinal=0;
    uint32 Flags=0;
    bool Negative=false, Behind=false, Lazy=false;
};
enum class EOp:uint8 { Atom, Split, Jump, Mark, Capture, Reference, Conditional, Assert, Atomic, EnterLoop, Loop, EndLoop, SearchStart, Accept };
struct FInstruction
{
    EOp Op=EOp::Accept;
    int A=0,B=0,C=0,D=0;
    bool Negative=false, Reverse=false, Lazy=false;
};
struct FAtom
{
    URegularExpression* Regex=nullptr;
    ~FAtom(){if(Regex)uregex_close(Regex);}
};
bool Decimal(const FString& S)
{
    if(S.IsEmpty())return false;
    for(TCHAR C:S)if(C<'0'||C>'9')return false;
    return true;
}
bool Identifier(const FString& S)
{
    if(S.IsEmpty())return false;
    for(TCHAR C:S)if(!FChar::IsAlnum(C)&&C!='_')return false;
    return true;
}
}
struct FProgram
{
    TArray<FInstruction> Code;
    TArray<TSharedPtr<FAtom>> Atoms;
    TMap<FString,int,FDefaultSetAllocator,TACERegexExactKeys<int>> Names;
    TSet<int> Groups;
    int Entry=0;
};
namespace
{
class FParser
{
    const FString& Pattern;
    FString& Error;
    int Pos=0, Depth=0, Unnamed=0, CaptureNumber=0, Loops=0, Declared=0;
    bool IgnoreNextUnnamed=false;
    uint32 Flags=0;
    TArray<FNode> Nodes;
    TSharedPtr<FProgram> Program=MakeShared<FProgram>();
    int Add(FNode Node)
    {
        if(Nodes.Num()>=2048){Fail(TEXT("Regex exceeds 2048 syntax nodes"));return 0;}
        return Nodes.Add(MoveTemp(Node));
    }
    void Fail(const TCHAR* Reason){if(Error.IsEmpty())Error=FString::Printf(TEXT("%s at %d"),Reason,Pos);}
    bool Take(TCHAR C){if(Pos<Pattern.Len()&&Pattern[Pos]==C){++Pos;return true;}return false;}
    int Expression()
    {
        FNode N;N.Kind=ENode::Alternative;N.Children.Add(Sequence());
        while(Error.IsEmpty()&&Take('|'))N.Children.Add(Sequence());
        return N.Children.Num()==1?N.Children[0]:Add(MoveTemp(N));
    }
    int Sequence()
    {
        FNode N;N.Kind=ENode::Sequence;
        while(Error.IsEmpty()&&Pos<Pattern.Len()&&Pattern[Pos]!=')'&&Pattern[Pos]!='|')
        {
            int Child=Primary();
            if(!Error.IsEmpty())break;
            int Min=0,Max=0;bool Quantified=true;
            if(Take('*'))Max=MAX_int32;
            else if(Take('+')){Min=1;Max=MAX_int32;}
            else if(Take('?'))Max=1;
            else if(Take('{'))
            {
                const int Start=Pos;
                while(Pos<Pattern.Len()&&FChar::IsDigit(Pattern[Pos]))++Pos;
                int64 Value=FCString::Atoi64(*Pattern.Mid(Start,Pos-Start));
                if(Pos==Start||Value>MAX_int32){Fail(TEXT("Invalid regex repetition"));break;}
                Min=Max=int(Value);
                if(Take(','))
                {
                    int Begin=Pos;while(Pos<Pattern.Len()&&FChar::IsDigit(Pattern[Pos]))++Pos;
                    Value=Begin==Pos?MAX_int32:FCString::Atoi64(*Pattern.Mid(Begin,Pos-Begin));
                    if(Value>MAX_int32){Fail(TEXT("Invalid regex repetition"));break;}
                    Max=int(Value);
                }
                if(!Take('}')||Max<Min){Fail(TEXT("Invalid regex repetition"));break;}
            }
            else Quantified=false;
            if(Quantified){FNode R;R.Kind=ENode::Repeat;R.Children.Add(Child);R.Min=Min;R.Max=Max;R.Lazy=Take('?');Child=Add(MoveTemp(R));}
            N.Children.Add(Child);
        }
        return Add(MoveTemp(N));
    }
    int Primary()
    {
        if(Take('('))return Group();
        FNode N;N.Kind=ENode::Atom;N.Flags=Flags;
        int Start=Pos;
        if(Take('['))
        {
            Take('^');if(Pos<Pattern.Len()&&Pattern[Pos]==']')++Pos;
            while(Pos<Pattern.Len()&&Pattern[Pos]!=']'){if(Pattern[Pos]=='\\'&&Pos+1<Pattern.Len())++Pos;++Pos;}
            if(!Take(']'))Fail(TEXT("Unterminated character class"));
        }
        else if(Take('\\'))
        {
            if(Pos==Pattern.Len()){Fail(TEXT("Trailing escape"));return Add(MoveTemp(N));}
            TCHAR C=Pattern[Pos++];
            if(C=='k'&&Take('<'))
            {
                int Begin=Pos;while(Pos<Pattern.Len()&&Pattern[Pos]!='>')++Pos;
                N.Kind=ENode::Backref;N.Name=Pattern.Mid(Begin,Pos-Begin);
                if(!Take('>'))Fail(TEXT("Unterminated backreference"));
            }
            else if(C>='1'&&C<='9')
            {
                while(Pos<Pattern.Len()&&FChar::IsDigit(Pattern[Pos]))++Pos;
                N.Kind=ENode::Backref;N.Name=Pattern.Mid(Start+1,Pos-Start-1);N.Text=TEXT("numeric");
            }
            else if((C=='p'||C=='P'||C=='x')&&Take('{'))
            {
                while(Pos<Pattern.Len()&&Pattern[Pos]!='}')++Pos;
                if(!Take('}'))Fail(TEXT("Unterminated escape"));
            }
            else if(C=='G')N.Kind=ENode::SearchStart;
            else if(C=='c'&&Pos<Pattern.Len())++Pos;
            else if(C=='0'){int Count=1;while(Count<3&&Pos<Pattern.Len()&&Pattern[Pos]>='0'&&Pattern[Pos]<='7'){++Pos;++Count;}}
        }
        else
        {
            if(Pattern[Pos]=='*'||Pattern[Pos]=='+'||Pattern[Pos]=='?'||Pattern[Pos]=='{')Fail(TEXT("Quantifier without expression"));
            ++Pos;
            if(Pos<Pattern.Len()&&uint32(Pattern[Start])>=0xd800&&uint32(Pattern[Start])<=0xdbff&&uint32(Pattern[Pos])>=0xdc00&&uint32(Pattern[Pos])<=0xdfff)++Pos;
        }
        if(N.Kind==ENode::Atom)N.Text=Pattern.Mid(Start,Pos-Start);
        return Add(MoveTemp(N));
    }
    int Group()
    {
        if(++Depth>MaxDepth){Fail(TEXT("Regex nesting exceeds 64"));return 0;}
        const uint32 Saved=Flags;
        FNode N;N.Kind=ENode::Capture;N.Flags=Flags;N.Ordinal=++Declared;
        if(Take('?'))
        {
            if(Take(':'))N.Kind=ENode::Sequence;
            else if(Take('>'))N.Kind=ENode::Atomic;
            else if(Take('=')){N.Kind=ENode::Look;}
            else if(Take('!')){N.Kind=ENode::Look;N.Negative=true;}
            else if(Take('<'))
            {
                if(Take('=')){N.Kind=ENode::Look;N.Behind=true;}
                else if(Take('!')){N.Kind=ENode::Look;N.Behind=true;N.Negative=true;}
                else
                {
                    int Begin=Pos;while(Pos<Pattern.Len()&&Pattern[Pos]!='>')++Pos;
                    FString Spec=Pattern.Mid(Begin,Pos-Begin);
                    if(!Take('>'))Fail(TEXT("Unterminated capture name"));
                    int Dash=INDEX_NONE;
                    if(Spec.FindChar('-',Dash)){N.Kind=ENode::Balance;N.Name=Spec.Left(Dash);N.Pop=Spec.Mid(Dash+1);if(!Identifier(N.Pop))Fail(TEXT("Invalid balancing group"));}
                    else N.Name=Spec;
                    if((N.Name.IsEmpty()&&N.Kind!=ENode::Balance)||(!N.Name.IsEmpty()&&!Identifier(N.Name)))Fail(TEXT("Invalid capture name"));
                }
            }
            else if(Take('('))
            {
                N.Kind=ENode::Conditional;
                int Close=Pattern.Find(TEXT(")"),ESearchCase::CaseSensitive,ESearchDir::FromStart,Pos);
                FString Token=Close==INDEX_NONE?FString():Pattern.Mid(Pos,Close-Pos);
                if(Identifier(Token)){N.Name=Token;Pos=Close+1;}
                else
                {
                    FNode Condition;Condition.Kind=ENode::Look;
                    IgnoreNextUnnamed=false;
                    if(Take('?'))
                    {
                        // .NET's expression-conditional parser suppresses the
                        // next ordinary capture after an assertion prefix.
                        // Its capture-slot prepass still reserves that slot.
                        IgnoreNextUnnamed=true;
                        if(Take('<'))Condition.Behind=true;
                        if(Take('!'))Condition.Negative=true;
                        else if(!Take('='))Fail(TEXT("Invalid conditional assertion"));
                    }
                    Condition.Children.Add(Expression());
                    if(!Take(')'))Fail(TEXT("Unterminated conditional assertion"));
                    N.Children.Add(Add(MoveTemp(Condition)));
                }
                N.Children.Add(Sequence());
                if(Take('|'))N.Children.Add(Sequence());else N.Children.Add(Add(FNode{}));
                if(!Take(')'))Fail(TEXT("Conditional requires at most two branches"));
                Flags=Saved;--Depth;return Add(MoveTemp(N));
            }
            else
            {
                bool Disable=false,Any=false;
                while(Pos<Pattern.Len())
                {
                    TCHAR C=Pattern[Pos];uint32 Bit=0;
                    if(C=='-'){Disable=true;++Pos;continue;}
                    if(C=='i')Bit=UREGEX_CASE_INSENSITIVE;else if(C=='m')Bit=UREGEX_MULTILINE;else if(C=='s')Bit=UREGEX_DOTALL;else break;
                    Any=true;Flags=Disable?(Flags&~Bit):(Flags|Bit);++Pos;
                }
                if(!Any){Fail(TEXT("Invalid group prefix"));return 0;}
                if(Take(')')){--Depth;return Add(FNode{});}
                if(!Take(':'))Fail(TEXT("Invalid option group"));
                N.Kind=ENode::Sequence;
            }
        }
        else
        {
            ++Unnamed;
            if(IgnoreNextUnnamed){N.Kind=ENode::Sequence;IgnoreNextUnnamed=false;}
            else N.Group=++CaptureNumber;
        }
        N.Children.Add(Expression());
        if(!Take(')'))Fail(TEXT("Unterminated group"));
        Flags=Saved;--Depth;return Add(MoveTemp(N));
    }
    int Reference(const FString& Name)const
    {
        if(Decimal(Name)){int G=Name.Len()<4?FCString::Atoi(*Name):-1;return Program->Groups.Contains(G)?G:-1;}
        const int* G=Program->Names.Find(Name);return G?*G:-1;
    }
    int Emit(EOp Op,int A=0,int B=0,int C=0,int D=0)
    {
        FInstruction I;I.Op=Op;I.A=A;I.B=B;I.C=C;I.D=D;return Program->Code.Add(I);
    }
    int Subprogram(int Node,bool Reverse)
    {
        int Skip=Emit(EOp::Jump),Entry=Program->Code.Num();Write(Node,Reverse);Emit(EOp::Accept);Program->Code[Skip].A=Program->Code.Num();return Entry;
    }
    void Write(int Index,bool Reverse)
    {
        if(!Error.IsEmpty())return;
        const FNode& N=Nodes[Index];
        switch(N.Kind)
        {
        case ENode::Empty:break;
        case ENode::SearchStart:Emit(EOp::SearchStart);break;
        case ENode::Atom:
        {
            auto Atom=MakeShared<FAtom>();UErrorCode Status=U_ZERO_ERROR;UParseError Parse{};
            const auto UTF=StringCast<UTF16CHAR>(*N.Text,N.Text.Len());
            Atom->Regex=uregex_open(reinterpret_cast<const UChar*>(UTF.Get()),UTF.Length(),N.Flags,&Parse,&Status);
            if(U_FAILURE(Status)){Fail(TEXT("Invalid regex atom"));break;}
            int I=Emit(EOp::Atom,Program->Atoms.Add(Atom));Program->Code[I].Reverse=Reverse;break;
        }
        case ENode::Sequence:
            if(Reverse){for(int I=N.Children.Num()-1;I>=0;--I)Write(N.Children[I],Reverse);}else for(int C:N.Children)Write(C,Reverse);
            break;
        case ENode::Alternative:
        {
            TArray<int> Jumps;
            for(int I=0;I<N.Children.Num();++I)
            {
                int Split=I+1<N.Children.Num()?Emit(EOp::Split):-1;
                if(Split>=0)Program->Code[Split].A=Program->Code.Num();
                Write(N.Children[I],Reverse);
                if(Split>=0){Jumps.Add(Emit(EOp::Jump));Program->Code[Split].B=Program->Code.Num();}
            }
            for(int J:Jumps)Program->Code[J].A=Program->Code.Num();break;
        }
        case ENode::Capture:case ENode::Balance:
            Emit(EOp::Mark,N.Subtract);Write(N.Children[0],Reverse);Emit(EOp::Capture,N.Group,N.Subtract);break;
        case ENode::Backref:
        {int I=Emit(EOp::Reference,N.Group,int(N.Flags));Program->Code[I].Reverse=Reverse;break;}
        case ENode::Conditional:
        {
            int ConditionEntry=-1;
            if(N.Children.Num()==3)ConditionEntry=Subprogram(N.Children[0],Reverse);
            int Branch=Emit(EOp::Conditional,N.Group,ConditionEntry);
            Program->Code[Branch].C=Program->Code.Num();Write(N.Children[N.Children.Num()-2],Reverse);
            int Jump=Emit(EOp::Jump);Program->Code[Branch].D=Program->Code.Num();Write(N.Children.Last(),Reverse);Program->Code[Jump].A=Program->Code.Num();break;
        }
        case ENode::Look:case ENode::Atomic:
        {
            int Entry=Subprogram(N.Children[0],N.Kind==ENode::Look?N.Behind:Reverse);
            int I=Emit(N.Kind==ENode::Look?EOp::Assert:EOp::Atomic,Entry);Program->Code[I].Negative=N.Negative;break;
        }
        case ENode::Repeat:
        {
            int Loop=Loops++;if(Loop>=MaxLoops){Fail(TEXT("Regex exceeds 32 repetitions"));break;}
            Emit(EOp::EnterLoop,Loop);int Check=Emit(EOp::Loop,Loop,N.Min,N.Max);Program->Code[Check].Lazy=N.Lazy;
            Write(N.Children[0],Reverse);Emit(EOp::EndLoop,Loop,Check);Program->Code[Check].D=Program->Code.Num();break;
        }
        }
    }
public:
    FParser(const FString& In,FString& Out):Pattern(In),Error(Out){}
    TSharedPtr<const FProgram> Parse()
    {
        int Root=Expression();if(Pos!=Pattern.Len())Fail(TEXT("Unexpected closing parenthesis"));
        if(!Error.IsEmpty())return nullptr;
        for(int G=1;G<=Unnamed;++G)Program->Groups.Add(G);
        // Explicit numbered groups reserve their slots before named groups are
        // allocated. Repeated names deliberately share one capture stack.
        for(const FNode& N:Nodes)if((N.Kind==ENode::Capture||N.Kind==ENode::Balance)&&Decimal(N.Name))
        {int G=N.Name.Len()<4?FCString::Atoi(*N.Name):MAX_int32;if(G<=0||G>MaxGroups){Fail(TEXT("Capture number exceeds 32"));return nullptr;}Program->Groups.Add(G);}
        int Next=Unnamed+1;
        TArray<int> Named;
        for(int I=0;I<Nodes.Num();++I)if((Nodes[I].Kind==ENode::Capture||Nodes[I].Kind==ENode::Balance)&&!Nodes[I].Name.IsEmpty())Named.Add(I);
        Named.Sort([&](int A,int B){return Nodes[A].Ordinal<Nodes[B].Ordinal;});
        for(int I:Named)
        {
            FNode& N=Nodes[I];
            if(Decimal(N.Name))N.Group=FCString::Atoi(*N.Name);
            else if(const int* Existing=Program->Names.Find(N.Name))N.Group=*Existing;
            else{while(Program->Groups.Contains(Next))++Next;N.Group=Next++;Program->Names.Add(N.Name,N.Group);Program->Groups.Add(N.Group);}
        }
        for(int G:Program->Groups)if(G>MaxGroups){Fail(TEXT("Pattern exceeds 32 capture groups"));return nullptr;}
        // Resolve before emitting; assertion-expression conditionals with an
        // undeclared word are literal lookaheads in .NET, not missing groups.
        const int Count=Nodes.Num();
        for(int I=0;I<Count;++I)
        {
            if(Nodes[I].Kind==ENode::Balance){Nodes[I].Subtract=Reference(Nodes[I].Pop);if(Nodes[I].Subtract<0)Fail(TEXT("Undefined balancing capture"));}
            if(Nodes[I].Kind==ENode::Backref)
            {
                Nodes[I].Group=Reference(Nodes[I].Name);
                if(Nodes[I].Group<0)
                {
                    FString Digits=Nodes[I].Name;
                    if(Nodes[I].Text!=TEXT("numeric")||Digits.Len()<2||Digits[0]>'7'){Fail(TEXT("Undefined backreference"));continue;}
                    int Octal=0,K=0;while(K<3&&K<Digits.Len()&&Digits[K]<='7')Octal=Octal*8+Digits[K++]-'0';
                    Nodes[I].Kind=ENode::Atom;Nodes[I].Text=FString::Printf(TEXT("\\x{%X}"),Octal&255)+Digits.Mid(K);
                }
            }
            if(Nodes[I].Kind==ENode::Conditional&&!Nodes[I].Name.IsEmpty())
            {
                Nodes[I].Group=Reference(Nodes[I].Name);
                if(Nodes[I].Group<0)
                {
                    if(Decimal(Nodes[I].Name)){Fail(TEXT("Undefined conditional capture"));continue;}
                    FNode Atom;Atom.Kind=ENode::Atom;Atom.Text=Nodes[I].Name;Atom.Flags=Nodes[I].Flags;
                    int A=Add(MoveTemp(Atom));FNode Look;Look.Kind=ENode::Look;Look.Children.Add(A);int L=Add(MoveTemp(Look));Nodes[I].Children.Insert(L,0);
                }
            }
        }
        Program->Entry=Program->Code.Num();Write(Root,false);Emit(EOp::Accept);
        return Error.IsEmpty()?Program:nullptr;
    }
};

// Capture histories and group-start marks live in a bounded append-only arena.
// Choice points copy only indices. Restoring a choice restores both the capture
// stacks and arena length, including captures removed by balancing groups.
struct FRecord {int Start=0,End=0,Previous=-1;};
struct FState
{
    int Pc=0,Pos=0,Mark=-1;
    int Top[MaxGroups+1];
    int Count[MaxLoops]{},Last[MaxLoops]{};
    FState(){for(int& T:Top)T=-1;}
};
struct FChoice {FState State;int Records=0;};
class FMatcher
{
    const FProgram& Program;
    FString& Error;
    TArray<UChar> Text;
    TArray<URegularExpression*> Atoms;
    TArray<FRecord> Records;
    double Deadline=0;
    int Steps=0,StackBytes=0;
    bool Work()
    {
        if(!Error.IsEmpty())return false;
        if(++Steps>100000||FPlatformTime::Seconds()>Deadline)Error=TEXT("Regex exceeded its 2 ms work limit");
        return Error.IsEmpty();
    }
    int Record(int Start,int End,int Previous)
    {
        if((Records.Num()+1)*sizeof(FRecord)>MaxBytes){Error=TEXT("Regex exceeded its 64 KiB capture-history limit");return -1;}
        return Records.Add({Start,End,Previous});
    }
    bool Atom(int Id,int& Pos,bool Reverse)
    {
        UErrorCode Status=U_ZERO_ERROR;
        if(!Atoms[Id])
        {
            auto* R=uregex_clone(Program.Atoms[Id]->Regex,&Status);Atoms[Id]=R;
            if(U_SUCCESS(Status)){uregex_setTimeLimit(R,2,&Status);uregex_setStackLimit(R,MaxBytes,&Status);uregex_setText(R,Text.GetData(),Text.Num(),&Status);}
        }
        auto* R=Atoms[Id];bool Found=false;
        if(U_SUCCESS(Status))
        {
            if(!Reverse){Found=uregex_lookingAt(R,Pos,&Status)!=0;if(Found)Pos=uregex_end(R,0,&Status);}
            else
            {
                // Atoms are a character/class, a zero-width anchor, or a
                // literal conditional token. Try the nearest starting point
                // first; the surrounding program runs right-to-left.
                for(int Start=Pos;Start>=0&&Work();--Start)
                {
                    if(Start>0&&Start<Text.Num()&&U16_IS_TRAIL(Text[Start])&&U16_IS_LEAD(Text[Start-1]))continue;
                    if(uregex_lookingAt(R,Start,&Status)&&uregex_end(R,0,&Status)==Pos){Pos=Start;Found=true;break;}
                    if(U_FAILURE(Status))break;
                }
            }
        }
        if(U_FAILURE(Status))Error=FString::Printf(TEXT("Regex atom failed within its work limit: %s"),UTF8_TO_TCHAR(u_errorName(Status)));
        return Found;
    }
    bool Run(FState& State,int Entry,int Depth)
    {
        if(Depth>MaxDepth){Error=TEXT("Regex assertion nesting exceeds 64");return false;}
        // Account for recursive states as well as explicit backtracking frames.
        StackBytes+=sizeof(FState);
        if(StackBytes>MaxBytes){StackBytes-=sizeof(FState);Error=TEXT("Regex exceeded its 64 KiB stack limit");return false;}
        TArray<FChoice> Choices;
        struct FRelease{int& Bytes;TArray<FChoice>& Choices;~FRelease(){Bytes-=sizeof(FState)+Choices.Num()*sizeof(FChoice);}} Release{StackBytes,Choices};
        State.Pc=Entry;
        auto Save=[&](const FState& Alternative)
        {
            if(StackBytes+sizeof(FChoice)>MaxBytes){Error=TEXT("Regex exceeded its 64 KiB stack limit");return;}
            Choices.Add({Alternative,Records.Num()});StackBytes+=sizeof(FChoice);
        };
        while(Work())
        {
            const FInstruction& I=Program.Code[State.Pc++];bool OK=true;
            switch(I.Op)
            {
            case EOp::Accept:return true;
            case EOp::Atom:OK=Atom(I.A,State.Pos,I.Reverse);break;
            case EOp::SearchStart:OK=State.Pos==0;break;
            case EOp::Split:{FState Other=State;Other.Pc=I.B;Save(Other);State.Pc=I.A;break;}
            case EOp::Jump:State.Pc=I.A;break;
            case EOp::Mark:
                if(I.A>=0&&State.Top[I.A]<0)OK=false;
                else State.Mark=Record(State.Pos,State.Pos,State.Mark);
                break;
            case EOp::Capture:
            {
                if(State.Mark<0){OK=false;break;}
                int Start=FMath::Min(Records[State.Mark].Start,State.Pos),End=FMath::Max(Records[State.Mark].Start,State.Pos);
                State.Mark=Records[State.Mark].Previous;
                if(I.B>=0)
                {
                    int Old=State.Top[I.B];if(Old<0){OK=false;break;}
                    const FRecord Prior=Records[Old];State.Top[I.B]=Prior.Previous;
                    if(Start>=Prior.End){End=Start;Start=Prior.End;}
                    else if(End<=Prior.Start){Start=Prior.Start;End=FMath::Max(Start,End);}
                    else{Start=FMath::Max(Start,Prior.Start);End=FMath::Min(End,Prior.End);}
                }
                if(I.A>=0)State.Top[I.A]=Record(Start,End,State.Top[I.A]);break;
            }
            case EOp::Reference:
            {
                int Top=State.Top[I.A];if(Top<0){OK=false;break;}
                FRecord R=Records[Top];int Length=R.End-R.Start,Begin=I.Reverse?State.Pos-Length:State.Pos;
                OK=Begin>=0&&Begin+Length<=Text.Num();
                if(OK)
                {
                    if(I.B&UREGEX_CASE_INSENSITIVE){UErrorCode Status=U_ZERO_ERROR;OK=u_strCaseCompare(Text.GetData()+Begin,Length,Text.GetData()+R.Start,Length,U_FOLD_CASE_DEFAULT,&Status)==0&&U_SUCCESS(Status);}
                    else OK=FMemory::Memcmp(Text.GetData()+Begin,Text.GetData()+R.Start,Length*sizeof(UChar))==0;
                }
                if(OK)State.Pos+=I.Reverse?-Length:Length;break;
            }
            case EOp::Conditional:
            {
                bool Yes=I.A>=0?State.Top[I.A]>=0:false;
                if(I.B>=0)
                {
                    FState Trial=State;int Saved=Records.Num();Yes=Run(Trial,I.B,Depth+1);
                    if(Yes){int PC=State.Pc,Position=State.Pos;State=Trial;State.Pc=PC;State.Pos=Position;}else Records.SetNum(Saved,EAllowShrinking::No);
                }
                State.Pc=Yes?I.C:I.D;break;
            }
            case EOp::Assert:case EOp::Atomic:
            {
                FState Trial=State;int Saved=Records.Num();bool Found=Run(Trial,I.A,Depth+1);OK=I.Negative?!Found:Found;
                if(Found&&!I.Negative){int PC=State.Pc,Position=State.Pos;State=Trial;State.Pc=PC;if(I.Op==EOp::Assert)State.Pos=Position;}
                else Records.SetNum(Saved,EAllowShrinking::No);
                break;
            }
            case EOp::EnterLoop:State.Count[I.A]=0;State.Last[I.A]=-1;break;
            case EOp::Loop:
            {
                int Count=State.Count[I.A];
                if(Count>=I.C||(Count>=I.B&&State.Last[I.A]==State.Pos)){State.Pc=I.D;break;}
                if(Count>=I.B)
                {
                    FState Other=State;
                    if(I.Lazy){Other.Last[I.A]=State.Pos;Save(Other);State.Pc=I.D;break;}
                    Other.Pc=I.D;Save(Other);
                }
                State.Last[I.A]=State.Pos;break;
            }
            case EOp::EndLoop:++State.Count[I.A];State.Pc=I.B;break;
            }
            if(!OK)
            {
                if(Choices.IsEmpty())return false;
                FChoice Choice=Choices.Pop(EAllowShrinking::No);StackBytes-=sizeof(FChoice);State=Choice.State;Records.SetNum(Choice.Records,EAllowShrinking::No);
            }
        }
        return false;
    }
public:
    FMatcher(const FProgram& In,const FString& Input,FString& Out):Program(In),Error(Out)
    {
        auto UTF=StringCast<UTF16CHAR>(*Input,Input.Len());Text.Append(reinterpret_cast<const UChar*>(UTF.Get()),UTF.Length());
        // ICU permits zero-length input but still requires a non-null pointer.
        Text.Reserve(FMath::Max(1,Text.Num()));Atoms.SetNumZeroed(Program.Atoms.Num());
    }
    ~FMatcher(){for(auto* A:Atoms)if(A)uregex_close(A);}
    bool Find(ACEVTRegex::FCaptures* Captures)
    {
        Deadline=FPlatformTime::Seconds()+0.002;
        for(int Start=0;Start<=Text.Num()&&Work();++Start)
        {
            if(Start>0&&Start<Text.Num()&&U16_IS_TRAIL(Text[Start])&&U16_IS_LEAD(Text[Start-1]))continue;
            Records.Reset();FState State;State.Pos=Start;
            if(!Run(State,Program.Entry,0))continue;
            if(!Error.IsEmpty())return false;
            if(Captures)
            {
                auto Value=[&](int Begin,int End){auto S=StringCast<TCHAR>(reinterpret_cast<const UTF16CHAR*>(Text.GetData()+Begin),End-Begin);return FString(S.Length(),S.Get());};
                Captures->Add(TEXT("0"),Value(Start,State.Pos));
                for(int G:Program.Groups)if(State.Top[G]>=0){const FRecord& R=Records[State.Top[G]];Captures->Add(FString::FromInt(G),Value(R.Start,R.End));}
                for(const auto& Pair:Program.Names)if(State.Top[Pair.Value]>=0){const FRecord& R=Records[State.Top[Pair.Value]];Captures->Add(Pair.Key,Value(R.Start,R.End));}
            }
            return true;
        }
        return false;
    }
};
}
TSharedPtr<const FProgram> Compile(const FString& Pattern,FString& Error){return FParser(Pattern,Error).Parse();}
bool Match(const FProgram& Program,const FString& Text,ACEVTRegex::FCaptures* Captures,FString& Error){return FMatcher(Program,Text,Error).Find(Captures);}
#else
struct FProgram {};
TSharedPtr<const FProgram> Compile(const FString&,FString& Error){Error=TEXT("This build lacks ICU regex support");return nullptr;}
bool Match(const FProgram&,const FString&,ACEVTRegex::FCaptures*,FString& Error){Error=TEXT("This build lacks ICU regex support");return false;}
#endif
}
