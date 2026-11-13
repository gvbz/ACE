#include "ACEVTProfile.h"
#include "ACEVTRegex.h"
#include "ACEUCMLootPresentation.h"
#include "ACEClientChatCommands.h"
#include "String/LexFromString.h"

namespace ACEVTProfile
{
namespace
{
using J=TSharedPtr<FJsonObject>;
using V=TSharedPtr<FJsonValue>;
using A=TArray<V>;
V Obj(const J& O){return MakeShared<FJsonValueObject>(O);}
V Str(const FString& S){return MakeShared<FJsonValueString>(S);}
bool VTProperty(double Key,bool Float)
{
    // All server keys and Decal aliases remain addressable. Like Classic, a
    // missing numeric property compares as zero; custom server keys are valid.
    return Key>0&&Key<=MAX_int32&&FMath::FloorToDouble(Key)==Key;
}
struct Reader
{
    const FString& Text;int32 At=0,Nodes=0;FString Error;
    FString Line()
    {
        if(At>=Text.Len()){Error=TEXT("Unexpected end of file");return {};}
        const int32 Start=At;while(At<Text.Len()&&Text[At]!='\n'&&Text[At]!='\r')++At;
        const FString S=Text.Mid(Start,At-Start);
        if(At<Text.Len()&&Text[At]=='\r')++At;if(At<Text.Len()&&Text[At]=='\n')++At;return S;
    }
    double Number()
    {double N=0;const FString S=Line();if(!LexTryParseString(N,*S)||!FMath::IsFinite(N))Error=TEXT("Invalid number");return N;}
    int32 Count(int32 Max=10000)
    {const double N=Number();if(N<0||N>Max||FMath::FloorToDouble(N)!=N){Error=TEXT("Invalid count");return 0;}return int32(N);}
    FString Block(bool AllowFinalNewline=false)
    {
        const int32 N=Count(4*1024*1024);
        // Some Classic exports lose the final CRLF in transport. Recover only
        // that terminator on the final named policy, never a truncated rule.
        const int32 Remaining=Text.Len()-At;
        if(AllowFinalNewline&&N==Remaining+2&&Remaining>0&&Text[Text.Len()-1]!='\n'&&Text[Text.Len()-1]!='\r')
        {FString S=Text.Mid(At)+TEXT("\r\n");At=Text.Len();return S;}
        if(N>Remaining){Error=TEXT("Truncated length-prefixed block");return {};}
        FString S=Text.Mid(At,N);At+=N;return S;
    }
    J Table(int32 Depth)
    {
        auto O=MakeShared<FJsonObject>();const int32 Columns=Count(128);A Names,Flags,Rows;
        for(int I=0;I<Columns&&Error.IsEmpty();++I)Names.Add(Str(Line()));
        for(int I=0;I<Columns&&Error.IsEmpty();++I)Flags.Add(Str(Line()));
        const int32 N=Count(10000);
        for(int I=0;I<N&&Error.IsEmpty();++I){A Row;for(int C=0;C<Columns&&Error.IsEmpty();++C)Row.Add(Value(Depth+1));Rows.Add(MakeShared<FJsonValueArray>(Row));}
        O->SetArrayField(TEXT("columns"),Names);O->SetArrayField(TEXT("flags"),Flags);O->SetArrayField(TEXT("rows"),Rows);return O;
    }
    V Value(int32 Depth=0)
    {
        if(Depth>32||++Nodes>200000){Error=TEXT("Nested data limit exceeded");return MakeShared<FJsonValueNull>();}
        const FString Type=Line();
        if(Type==TEXT("n"))return MakeShared<FJsonValueNull>();
        if(Type==TEXT("s"))return Str(Line());
        if(Type==TEXT("b")){const FString S=Line().ToLower();if(S!=TEXT("true")&&S!=TEXT("false"))Error=TEXT("Invalid boolean");return MakeShared<FJsonValueBoolean>(S==TEXT("true"));}
        if(Type==TEXT("i")||Type==TEXT("u")||Type==TEXT("d")||Type==TEXT("f"))return MakeShared<FJsonValueNumber>(Number());
        if(Type==TEXT("TABLE"))return Obj(Table(Depth));
        if(Type==TEXT("ba")){auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("blob"),Block());return Obj(O);}
        Error=TEXT("Unknown typed value: ")+Type;return MakeShared<FJsonValueNull>();
    }
};
double N(const J& O,const TCHAR* Key,double Default=0){double V;return O->TryGetNumberField(Key,V)?V:Default;}
FString S(const J& O,const TCHAR* Key){FString V;O->TryGetStringField(Key,V);return V;}
}
J Read(const FString& Text,const FString& Extension,FString& Error)
{
    Error.Empty();if(Text.Len()>4*1024*1024){Error=TEXT("Profile exceeds 4 MiB");return nullptr;}
    Reader R{Text};auto D=MakeShared<FJsonObject>();const FString Ext=Extension.ToLower();D->SetStringField(TEXT("format"),Ext);
    if(Ext==TEXT("utl"))
    {
        const FString Magic=R.Line();int32 Version=0,Count=0;
        if(Magic==TEXT("UTL")){Version=R.Count(1);Count=R.Count();}
        else if(!LexTryParseString(Count,*Magic)||Count<0||Count>10000)R.Error=TEXT("Invalid UTL header");
        D->SetNumberField(TEXT("version"),Version);A Rules;
        const TMap<int32,int32> Lines={{0,1},{1,2},{2,2},{3,2},{4,2},{5,2},{6,1},{7,1},{8,1},{9,3},{10,1},{11,2},{12,2},{13,2},{14,5},{15,6},{16,6},{17,2},{1000,2},{1001,1},{1002,1},{1003,1},{1004,3},{2000,1},{2001,1},{2003,2},{2005,2},{2006,1},{2007,1},{2008,3},{9999,1}};
        for(int I=0;I<Count&&R.Error.IsEmpty();++I)
        {
            auto Rule=MakeShared<FJsonObject>();Rule->SetStringField(TEXT("label"),R.Line());Rule->SetStringField(TEXT("expression"),Version?R.Line():FString());
            TArray<FString> Header;R.Line().ParseIntoArray(Header,TEXT(";"),false);
            if(Header.Num()<2){R.Error=TEXT("Invalid rule header");break;}TArray<int32> Codes;
            for(const auto& H:Header){int32 Code=0;if(!LexTryParseString(Code,*H)){R.Error=TEXT("Invalid rule code");break;}Codes.Add(Code);}
            if(!R.Error.IsEmpty())break;
            Rule->SetNumberField(TEXT("priority"),Codes[0]);Rule->SetNumberField(TEXT("action_id"),Codes[1]);
            if(Codes[1]==10)Rule->SetNumberField(TEXT("amount"),R.Count(MAX_int32));A Requirements;
            for(int C=2;C<Codes.Num()&&R.Error.IsEmpty();++C)
            {
                auto Q=MakeShared<FJsonObject>();Q->SetNumberField(TEXT("type"),Codes[C]);FString Body;
                if(Version)Body=R.Block();
                else if(const int32* L=Lines.Find(Codes[C])){for(int B=0;B<*L;++B)Body+=R.Line()+TEXT("\n");}
                else R.Error=TEXT("Unknown version-0 requirement without a length");
                Q->SetStringField(TEXT("body"),Body);Requirements.Add(Obj(Q));
            }
            Rule->SetArrayField(TEXT("requirements"),Requirements);Rules.Add(Obj(Rule));
        }
        D->SetArrayField(TEXT("rules"),Rules);A Extras;
        while(R.At<Text.Len()&&R.Error.IsEmpty())
        {
            const FString Name=R.Line();if(Name.IsEmpty()&&R.At==Text.Len())break;
            auto E=MakeShared<FJsonObject>();E->SetStringField(TEXT("name"),Name);E->SetStringField(TEXT("body"),R.Block(Name==TEXT("SalvageCombine")));Extras.Add(Obj(E));
        }
        D->SetArrayField(TEXT("extras"),Extras);
    }
    else if(Ext==TEXT("nav"))
    {
        if(R.Line()!=TEXT("uTank2 NAV 1.2"))R.Error=TEXT("Unsupported NAV header");
        const int32 Mode=R.Count(4);D->SetNumberField(TEXT("mode"),Mode);A Points;
        if(Mode==3){D->SetStringField(TEXT("target_name"),R.Line());D->SetNumberField(TEXT("target_id"),R.Number());}
        else
        {
            const int32 Count=R.Count();
            for(int I=0;I<Count&&R.Error.IsEmpty();++I)
            {
                auto P=MakeShared<FJsonObject>();const int32 Type=R.Count(9);P->SetNumberField(TEXT("type"),Type);
                for(const TCHAR* K:{TEXT("ew"),TEXT("ns"),TEXT("z"),TEXT("reserved")})P->SetNumberField(K,R.Number());
                A Args;const int32 Counts[]={0,1,1,1,1,2,6,6,0,3};
                for(int C=0;C<Counts[Type];++C)Args.Add(Str(R.Line()));P->SetArrayField(TEXT("args"),Args);Points.Add(Obj(P));
            }
        }
        D->SetArrayField(TEXT("points"),Points);
    }
    else if(Ext==TEXT("met")||Ext==TEXT("usd"))
    {
        A Tables;const int32 Count=R.Count(128);
        for(int I=0;I<Count&&R.Error.IsEmpty();++I){const FString Name=R.Line();auto T=R.Table(0);T->SetStringField(TEXT("name"),Name);Tables.Add(Obj(T));}
        D->SetArrayField(TEXT("tables"),Tables);
    }
    else R.Error=TEXT("Supported files: .utl, .nav, .met, .usd");
    if(R.Error.IsEmpty()&&!Text.Mid(R.At).TrimStartAndEnd().IsEmpty())R.Error=TEXT("Unexpected trailing data");
    if(!R.Error.IsEmpty()){Error=FString::Printf(TEXT("%s at character %d"),*R.Error,R.At);return nullptr;}return D;
}

FString WriteLoot(const J& D,FString& Error)
{
    Error.Empty();const A* Rules=nullptr;
    if(!D||S(D,TEXT("format"))!=TEXT("utl")||!D->TryGetArrayField(TEXT("rules"),Rules)||Rules->Num()>10000){Error=TEXT("Invalid loot document");return {};}
    FString Text=FString::Printf(TEXT("UTL\r\n1\r\n%d\r\n"),Rules->Num());
    auto Line=[&](const FString& Value){if(Value.Contains(TEXT("\r"))||Value.Contains(TEXT("\n")))Error=TEXT("A label or expression contains a newline");Text+=Value+TEXT("\r\n");};
    auto Block=[&](const FString& Value){Line(FString::FromInt(Value.Len()));Text+=Value;};
    for(const auto& V:*Rules)
    {
        if(V->Type!=EJson::Object){Error=TEXT("Invalid rule");return {};}
        const auto R=V->AsObject();const A* Reqs=nullptr;if(!R->TryGetArrayField(TEXT("requirements"),Reqs)){Error=TEXT("Missing requirements");return {};}
        Line(S(R,TEXT("label")));Line(S(R,TEXT("expression")));
        FString Codes=FString::Printf(TEXT("%d;%d"),int(N(R,TEXT("priority"))),int(N(R,TEXT("action_id"))));
        for(const auto& Q:*Reqs){if(Q->Type!=EJson::Object){Error=TEXT("Invalid requirement");return {};}Codes+=FString::Printf(TEXT(";%d"),int(N(Q->AsObject(),TEXT("type"))));}
        Line(Codes);if(N(R,TEXT("action_id"))==10)Line(FString::FromInt(int(N(R,TEXT("amount")))));
        const TMap<int32,FString> Layouts={{0,TEXT("s")},{1,TEXT("si")},{2,TEXT("ii")},{3,TEXT("ii")},{4,TEXT("di")},{5,TEXT("di")},{6,TEXT("d")},{7,TEXT("i")},{8,TEXT("i")},{9,TEXT("ssi")},{10,TEXT("d")},{11,TEXT("ii")},{12,TEXT("ii")},{13,TEXT("ii")},{14,TEXT("iiidd")},{15,TEXT("iiidds")},{16,TEXT("iiiddi")},{17,TEXT("ii")},{1000,TEXT("ii")},{1001,TEXT("i")},{1002,TEXT("i")},{1003,TEXT("i")},{1004,TEXT("iii")},{2000,TEXT("d")},{2001,TEXT("d")},{2003,TEXT("ii")},{2005,TEXT("di")},{2006,TEXT("d")},{2007,TEXT("i")},{2008,TEXT("ddd")},{9999,TEXT("b")}};
        for(const auto& Q:*Reqs)
        {
            const auto Req=Q->AsObject();const int Type=int(N(Req,TEXT("type")));const FString Body=S(Req,TEXT("body"));
            if(const FString* Layout=Layouts.Find(Type))
            {
                TArray<FString> Lines;Body.Replace(TEXT("\r"),TEXT("")).ParseIntoArray(Lines,TEXT("\n"),false);if(Lines.Num()&&Lines.Last().IsEmpty())Lines.Pop();
                bool Valid=Lines.Num()==Layout->Len();
                for(int I=0;Valid&&I<Lines.Num();++I)
                {
                    const TCHAR Kind=(*Layout)[I];double Value=0;
                    if(Kind=='b')Valid=Lines[I].Equals(TEXT("true"),ESearchCase::IgnoreCase)||Lines[I].Equals(TEXT("false"),ESearchCase::IgnoreCase);
                    else if(Kind!='s')Valid=LexTryParseString(Value,*Lines[I])&&FMath::IsFinite(Value)&&(Kind!='i'||(FMath::FloorToDouble(Value)==Value&&Value>=MIN_int32&&Value<=MAX_int32));
                }
                if(!Valid){Error=FString::Printf(TEXT("Rule '%s', requirement %d: invalid field count or value."),*S(R,TEXT("label")),Type);return {};}
            }
            Block(Body);
        }
    }
    const A* Extras=nullptr;if(D->TryGetArrayField(TEXT("extras"),Extras))for(const auto& E:*Extras)
    {if(E->Type!=EJson::Object){Error=TEXT("Invalid extra block");return {};}Line(S(E->AsObject(),TEXT("name")));Block(S(E->AsObject(),TEXT("body")));}
    if(Text.Len()>4*1024*1024)Error=TEXT("Loot file exceeds 4 MiB");
    if(Error.IsEmpty()&&!Read(Text,TEXT("utl"),Error))return {};
    return Error.IsEmpty()?Text:FString();
}
bool IsSupportedPattern(const FString& Pattern)
{
    FString Error;return ACEVTRegex::Validate(Pattern,Error);
}
FString NativeCommand(const FString& Text)
{
    FString Command=Text.TrimStartAndEnd();
    Command=ACEClientChatCommands::CanonicalLine(Command);
    // Use the established parser spelling while normalizing VT subcommands.
    if(Command.StartsWith(TEXT("/acueucm")) && (Command.Len()==8 || FChar::IsWhitespace(Command[8])))
        Command=TEXT("/ucm")+Command.Mid(8);
    // Only replace the command token; quoted chat text and item names are data.
    if(Command.Len()>=3&&Command.Left(3).Equals(TEXT("/vt"),ESearchCase::IgnoreCase)
        &&(Command.Len()==3||FChar::IsWhitespace(Command[3])))Command=TEXT("/ucm")+Command.Mid(3);
    if((Command.Equals(TEXT("/og summon off"),ESearchCase::IgnoreCase)||Command.Equals(TEXT("/og summon false"),ESearchCase::IgnoreCase)))return TEXT("/acueucm opt set SummonPets false");
    if((Command.Equals(TEXT("/og summon on"),ESearchCase::IgnoreCase)||Command.Equals(TEXT("/og summon true"),ESearchCase::IgnoreCase)))return TEXT("/acueucm opt set SummonPets true");
    for(const auto& Alias:TArray<TPair<FString,FString>>{{TEXT("/ucm settings loadchar "),TEXT("/ucm settings load ")},{TEXT("/ucm lootprofile load "),TEXT("/ucm looting load ")}})
        if(Command.StartsWith(Alias.Key,ESearchCase::IgnoreCase))return ACEClientChatCommands::CanonicalLine(Alias.Value+Command.Mid(Alias.Key.Len()));
    return ACEClientChatCommands::CanonicalLine(Command);
}
#include "ACEVTMeta.inl"
#include "ACEVTSettings.inl"
J CompileCommand(const FString& Text,TArray<FString>& Issues){return VTCommand(Text,Issues);}
bool SetMonsterMatch(const J& Rule,const FString& Text,FString& Error)
{
    Error.Empty();const FString Name=Text.TrimStartAndEnd();
    if(!Rule||Name.IsEmpty()||Name.Len()>256){Error=TEXT("Enter a mob name or expression (maximum 256 characters).");return false;}
    J Expression;
    if(Name.Contains(TEXT("=="))||Name.Contains(TEXT("!="))||Name.Contains(TEXT("#"))
        ||Name.Contains(TEXT("<"))||Name.Contains(TEXT(">"))||Name.Contains(TEXT("&&"))||Name.Contains(TEXT("||")))
    {
        VTExpression Parser{Name};Expression=Parser.Expr(1,0);Parser.Space();TArray<FString> Issues;
        // Native snapshots include live species/max-health and DAT species names.
        // Imported profile validation still requires its historical catalog.
        if(!Expression||!Parser.Error.IsEmpty()||Parser.At!=Name.Len())
        {Error=Parser.Error.IsEmpty()?TEXT("Invalid mob expression."):Parser.Error;return false;}
        if(!VTMonsterExpression(Expression,Issues,true))
        {Error=FString::Join(Issues,TEXT("; "));return false;}
    }
    Rule->SetStringField(TEXT("name"),Name);
    if(Expression)Rule->SetObjectField(TEXT("expression"),Expression);else Rule->RemoveField(TEXT("expression"));
    return true;
}
J Convert(const J& D,TArray<FString>& Issues)
{
    Issues.Reset();auto Out=MakeShared<FJsonObject>();if(!D){Issues.Add(TEXT("No parsed profile"));return Out;}
    const FString Format=S(D,TEXT("format"));
    if(Format==TEXT("nav"))
    {
        const int Mode=int(N(D,TEXT("mode")));A Route;
        if(Mode<1||Mode>4)Issues.Add(TEXT("Invalid route mode."));
        if(Mode==3){Out->SetStringField(TEXT("follow_name"),S(D,TEXT("target_name")));Out->SetNumberField(TEXT("follow_id"),N(D,TEXT("target_id")));}
        else {Out->SetStringField(TEXT("follow_name"),TEXT(""));Out->SetNumberField(TEXT("follow_id"),0);}
        Out->SetBoolField(TEXT("loop_route"),Mode==1);Out->SetBoolField(TEXT("reverse_route"),Mode==2);
        for(const auto& Value:D->GetArrayField(TEXT("points")))
        {
            auto P=Value->AsObject();auto Q=MakeShared<FJsonObject>();const int Type=int(N(P,TEXT("type")));
            const double X=N(P,TEXT("ew"))*240+24468,Y=N(P,TEXT("ns"))*240+24468;
            // Dungeon coordinates may extend beyond a nominal landblock edge.
            // Preserve their absolute position; NAV does not contain env-cell IDs.
            if(X< -192||X>49344||Y< -192||Y>49344){Issues.Add(TEXT("Waypoint lies outside the world coordinate range."));continue;}
            const int BX=FMath::Clamp(FMath::FloorToInt(X/192),0,255),BY=FMath::Clamp(FMath::FloorToInt(Y/192),0,255);
            const double LX=X-BX*192,LY=Y-BY*192;
            Q->SetNumberField(TEXT("cell"),uint32((BX<<24)|(BY<<16)|(FMath::Clamp(int(LX/24),0,7)*8+FMath::Clamp(int(LY/24),0,7)+1)));
            Q->SetNumberField(TEXT("x"),LX);Q->SetNumberField(TEXT("y"),LY);Q->SetNumberField(TEXT("z"),N(P,TEXT("z"))*240);
            const TCHAR* Kinds[]={TEXT("walk"),TEXT("portal"),TEXT("recall"),TEXT("pause"),TEXT("command"),TEXT("vendor"),TEXT("portal"),TEXT("use"),TEXT("checkpoint"),TEXT("jump")};
            Q->SetStringField(TEXT("kind"),Kinds[Type]);Q->SetBoolField(TEXT("legacy"),true);Q->SetBoolField(TEXT("walk_first"),Type==0||Type==1||Type==8);
            const auto Args=P->GetArrayField(TEXT("args"));
            auto NumberArg=[&](int Index,double Min,double Max){double V=0;if(!Args.IsValidIndex(Index)||!LexTryParseString(V,*Args[Index]->AsString())||!FMath::IsFinite(V)||V<Min||V>Max)Issues.Add(FString::Printf(TEXT("Waypoint %d: invalid argument %d."),Route.Num()+1,Index+1));return V;};
            if(Type==1)Q->SetNumberField(TEXT("object_id"),uint32(int64(NumberArg(0,MIN_int32,MAX_uint32))));
            if(Type==2)Q->SetNumberField(TEXT("spell"),NumberArg(0,1,MAX_int32));
            if(Type==4)Q->SetObjectField(TEXT("command"),VTCommand(Args[0]->AsString(),Issues));
            if(Type==5){Q->SetNumberField(TEXT("object_id"),uint32(int64(NumberArg(0,MIN_int32,MAX_uint32))));Q->SetStringField(TEXT("object_name"),Args[1]->AsString());}
            if(Type==6||Type==7)
            {
                Q->SetStringField(TEXT("object_name"),Args[0]->AsString());Q->SetNumberField(TEXT("object_class"),NumberArg(1,0,43));
                if(!Args[2]->AsString().Equals(TEXT("true"),ESearchCase::IgnoreCase))Issues.Add(TEXT("Portal/NPC position must be valid."));
                auto At=MakeShared<FJsonObject>();At->SetNumberField(TEXT("ew"),NumberArg(3,-102,102));At->SetNumberField(TEXT("ns"),NumberArg(4,-102,102));At->SetNumberField(TEXT("z"),NumberArg(5,-100,100));Q->SetObjectField(TEXT("object_coordinates"),At);
            }
            if(Type==9)
            {
                const double Heading=NumberArg(0,0,700);Q->SetNumberField(TEXT("heading"),FMath::Fmod(Heading+90,360));Q->SetBoolField(TEXT("current_heading"),Heading>360);
                const FString Shift=Args[1]->AsString().ToLower();if(Shift!=TEXT("true")&&Shift!=TEXT("false"))Issues.Add(TEXT("Invalid jump Shift flag."));Q->SetBoolField(TEXT("walk_jump"),Shift==TEXT("true"));
                FString Duration=Args[2]->AsString();int Dot=INDEX_NONE;Duration.FindChar('.',Dot);int Direction=3;
                if(Dot!=INDEX_NONE&&Duration.Len()-Dot==6&&Duration[Duration.Len()-1]>='3'&&Duration[Duration.Len()-1]<='5'){Direction=Duration[Duration.Len()-1]-'0';Duration.LeftChopInline(1);}
                double Millis=0;if(!LexTryParseString(Millis,*Duration)||!FMath::IsFinite(Millis)||Millis<0||Millis>10000)Issues.Add(TEXT("Invalid jump duration."));
                Q->SetNumberField(TEXT("charge"),FMath::Clamp(Millis/1000,0.,1.));Q->SetNumberField(TEXT("forward"),Direction==3?1:0);Q->SetNumberField(TEXT("strafe"),Direction==4?-1:Direction==5?1:0);
            }
            if(Type==3)
            {
                double Millis=0;
                if(Args.Num()!=1||!LexTryParseString(Millis,*Args[0]->AsString())||Millis<0||Millis>86400000)Issues.Add(TEXT("Invalid pause duration."));
                else Q->SetNumberField(TEXT("seconds"),Millis/1000);
            }
            Route.Add(Obj(Q));
        }
        if(Route.Num()>2048)Issues.Add(TEXT("Route exceeds 2048 waypoints."));
        Out->SetArrayField(TEXT("route"),Route);Out->SetBoolField(TEXT("navigation"),false);
    }
    else if(Format==TEXT("met"))return VTMeta(D,Issues);
    else if(Format==TEXT("utl"))
    {
        auto Combine=MakeShared<FJsonObject>();
        auto ParseRanges=[&](const FString& Text)
        {
            A Ranges;TArray<FString> Parts;Text.Replace(TEXT(";"),TEXT(",")).ParseIntoArray(Parts,TEXT(","));double Previous=-1;
            for(const FString& Part:Parts)
            {
                TArray<FString> Bounds;Part.ParseIntoArray(Bounds,TEXT("-"));double Low=0,High=0;
                if(Bounds.Num()<1||Bounds.Num()>2||!LexTryParseString(Low,*Bounds[0].TrimStartAndEnd())||!FMath::IsFinite(Low)) {Issues.Add(TEXT("Invalid salvage workmanship range: ")+Text);break;}
                High=Low;if(Bounds.Num()==2&&(!LexTryParseString(High,*Bounds[1].TrimStartAndEnd())||!FMath::IsFinite(High))){Issues.Add(TEXT("Invalid salvage workmanship range: ")+Text);break;}
                if(Low<0||High>10||High<Low||Low<Previous||Ranges.Num()>=32){Issues.Add(TEXT("Unordered/out-of-range salvage workmanship groups"));break;}
                Previous=High;auto Range=MakeShared<FJsonObject>();Range->SetNumberField(TEXT("min"),Low);Range->SetNumberField(TEXT("max"),High);Ranges.Add(Obj(Range));
            }
            return Ranges;
        };
        for(const auto& Extra:D->GetArrayField(TEXT("extras")))
        {
            const auto E=Extra->AsObject();if(S(E,TEXT("name"))!=TEXT("SalvageCombine")){Issues.Add(TEXT("Unknown loot policy block: ")+S(E,TEXT("name")));continue;}
            const FString Body=S(E,TEXT("body"));Reader R{Body};if(R.Count()!=1)Issues.Add(TEXT("Unsupported salvage combine version"));
            Combine->SetArrayField(TEXT("default"),ParseRanges(R.Line()));auto Materials=MakeShared<FJsonObject>(),Values=MakeShared<FJsonObject>();
            const int Count=R.Count(256);for(int I=0;I<Count&&R.Error.IsEmpty();++I){const int Material=R.Count(10000);Materials->SetArrayField(FString::FromInt(Material),ParseRanges(R.Line()));}
            if(R.At<Body.Len()){const int ValueCount=R.Count(256);for(int I=0;I<ValueCount&&R.Error.IsEmpty();++I){const int Material=R.Count(10000),Value=R.Count(MAX_int32);Values->SetNumberField(FString::FromInt(Material),Value);}}
            if(!R.Error.IsEmpty()||R.At!=Body.Len())Issues.Add(TEXT("Malformed salvage combine policy: ")+R.Error);
            Combine->SetObjectField(TEXT("materials"),Materials);Combine->SetObjectField(TEXT("values"),Values);
        }
        if(!Combine->Values.IsEmpty())Out->SetObjectField(TEXT("salvage_policy"),Combine);
        A Rules;
        for(const auto& V:D->GetArrayField(TEXT("rules")))
        {
            const auto R=V->AsObject();auto Q=MakeShared<FJsonObject>();A Conditions;TArray<FString> Problems;
            Q->SetStringField(TEXT("label"),S(R,TEXT("label")));Q->SetStringField(TEXT("action"),N(R,TEXT("action_id"))==0?TEXT("skip"):TEXT("keep"));
            const int Action=int(N(R,TEXT("action_id")));
            // VT counts matching display names (hv -> f9.a), including distinct
            // custom WCIDs that share a name, rather than counting only WCID.
            if(Action==10){Q->SetBoolField(TEXT("count_by_name"),true);Q->SetNumberField(TEXT("keep_up_to"),N(R,TEXT("amount")));if(N(R,TEXT("amount"))==0)Q->SetStringField(TEXT("action"),TEXT("skip"));}
            if(Action==2||Action==3||Action==4)Q->SetStringField(TEXT("action"),Action==2?TEXT("salvage"):Action==3?TEXT("sell"):TEXT("read"));
            if(Action<0||Action>10||(Action>=5&&Action<=9))Problems.Add(FString::Printf(TEXT("Custom User action %d requires its original plugin"),Action));
            // VTClassic cLootItemRule.Match/EarlyMatch evaluate IntRules only.
            // CustomExpression is persisted metadata, not an executable condition.
            bool Disabled=false;
            for(const auto& Req:R->GetArrayField(TEXT("requirements")))
                if(N(Req->AsObject(),TEXT("type"))==9999&&S(Req->AsObject(),TEXT("body")).TrimStartAndEnd().Equals(TEXT("true"),ESearchCase::IgnoreCase))Disabled=true;
            Q->SetBoolField(TEXT("enabled"),!Disabled);
            for(const auto& Req:R->GetArrayField(TEXT("requirements")))
            {
                auto C=MakeShared<FJsonObject>();auto Src=Req->AsObject();int Type=int(N(Src,TEXT("type")));
                TArray<FString> B;S(Src,TEXT("body")).Replace(TEXT("\r"),TEXT("")).ParseIntoArray(B,TEXT("\n"),false);
                if(B.Num()&&B.Last().IsEmpty())B.Pop();
                auto Num=[&](int I,double& V){return B.IsValidIndex(I)&&LexTryParseString(V,*B[I])&&FMath::IsFinite(V);};
                double Value=0,Key=0;bool Supported=true;
                if(Type==9999)continue;
                if(Type==2||Type==3||Type==4||Type==5||Type==11||Type==12||Type==13)
                {
                    Supported=B.Num()==2&&Num(0,Value)&&Num(1,Key)&&VTProperty(Key,Type==4||Type==5);
                    C->SetStringField(TEXT("field"),Type==4||Type==5?TEXT("float"):TEXT("int"));C->SetNumberField(TEXT("key"),Key);
                    C->SetNumberField(TEXT("value"),Value);C->SetBoolField(TEXT("missing_zero"),true);
                    C->SetStringField(TEXT("op"),Type==2||Type==4?TEXT("le"):Type==3||Type==5?TEXT("ge"):Type==11?TEXT("bits"):Type==12?TEXT("eq"):TEXT("ne"));
                }
                else if(Type==1)
                {
                    Supported=B.Num()==2&&Num(1,Key)&&VTProperty(Key,false)&&IsSupportedPattern(B[0]);
                    C->SetStringField(TEXT("field"),TEXT("string"));C->SetNumberField(TEXT("key"),Key);C->SetStringField(TEXT("pattern"),B.Num()?B[0]:TEXT(""));
                }
                else if(Type==0||Type==8||Type==9)
                {
                    Supported=(Type==0&&B.Num()==1&&IsSupportedPattern(B[0]))||(Type==8&&B.Num()==1&&Num(0,Value)&&Value>=0)||(Type==9&&B.Num()==3&&Num(2,Value)&&Value>=0&&IsSupportedPattern(B[0])&&IsSupportedPattern(B[1]));
                    // Classic SpellMatch succeeds inside the matching-spell loop,
                    // even when its saved Count is zero; SpellCount can match zero.
                    C->SetStringField(TEXT("field"),TEXT("spells"));C->SetNumberField(TEXT("value"),Type==0?1:Type==9?FMath::Max(1.,Value):Value);
                    C->SetStringField(TEXT("pattern"),Type==8?TEXT(""):(B.Num()?B[0]:TEXT("")));
                    C->SetStringField(TEXT("exclude"),Type==9&&B.Num()>1&&!B[1].TrimStartAndEnd().IsEmpty()?B[1]:TEXT(""));
                }
                else if(Type>=14&&Type<=16)
                {
                    const int Count=Type==14?5:6;Supported=B.Num()==Count;A Args;
                    for(int Index=0;Supported&&Index<(Type==15?5:Count);++Index){double Arg=0;Supported=Num(Index,Arg);Args.Add(MakeShared<FJsonValueNumber>(Arg));}
                    C->SetStringField(TEXT("field"),TEXT("legacy"));C->SetNumberField(TEXT("key"),Type);C->SetArrayField(TEXT("args"),Args);
                    if(Supported&&Type==15)
                    {
                        C->SetStringField(TEXT("group"),B[5]);A Slots;
                        if(const auto* Group=ACEUCMLootPresentation::ColorGroups().Find(B[5]))for(int Slot:*Group)Slots.Add(MakeShared<FJsonValueNumber>(Slot));
                        else Supported=false;
                        C->SetArrayField(TEXT("slots"),Slots);
                    }
                    Supported=Supported&&ACEUCMLootPresentation::ValidColorMatch(C);
                }
                else if(Type==6||Type==7||Type==10||Type==17||Type==1000||Type==1001||Type==1002||Type==1003||Type==1004||Type==2000||Type==2001||Type==2003||Type==2005||Type==2006||Type==2007||Type==2008)
                {
                    const int Count=(Type==1000||Type==17||Type==2003||Type==2005)?2:(Type==1004||Type==2008)?3:1;Supported=B.Num()==Count;A Args;
                    for(int Index=0;Supported&&Index<Count;++Index){double Argument=0;Supported=Num(Index,Argument);Args.Add(MakeShared<FJsonValueNumber>(Argument));}
                    if(Type==2003||Type==2005)Supported=Supported&&VTProperty(Args[1]->AsNumber(),Type==2005);
                    if(Type==17)Supported=Supported&&Args[0]->AsNumber()>=0&&Args[0]->AsNumber()<256&&Args[1]->AsNumber()>=0&&Args[1]->AsNumber()<=MAX_uint32;
                    if(Type==7)Supported=Supported&&Args[0]->AsNumber()>=0&&Args[0]->AsNumber()<=43;
                    C->SetStringField(TEXT("field"),TEXT("legacy"));C->SetNumberField(TEXT("key"),Type);C->SetArrayField(TEXT("args"),Args);
                }
                else Supported=false;
                if(!Supported)Problems.AddUnique(FString::Printf(TEXT("requirement %d or its expression/property mapping"),Type));
                else Conditions.Add(Obj(C));
            }
            Q->SetArrayField(TEXT("conditions"),Conditions);
            // Disabled Classic rules can retain requirements without a runtime adapter.
            // Do not let the native editor later enable a partially converted rule.
            if(Problems.Num()){A Pending;for(const auto& Problem:Problems)Pending.Add(Str(Problem));Q->SetArrayField(TEXT("compatibility_issues"),Pending);}
            if(Conditions.Num()>MaxLootConditions)Issues.Add(FString::Printf(TEXT("Rule %d (%s) exceeds %d conditions."),Rules.Num()+1,*S(R,TEXT("label")),MaxLootConditions));
            // Preserve original data separately. Never silently activate a partially converted rule set.
            if(!Disabled)for(const auto& P:Problems)Issues.Add(FString::Printf(TEXT("Rule %d (%s): %s"),Rules.Num()+1,*S(R,TEXT("label")),*P));
            Rules.Add(Obj(Q));
        }
        if(Rules.Num()>2048)Issues.Add(TEXT("Loot profile exceeds 2048 rules."));
        Out->SetArrayField(TEXT("loot_rules"),Rules);
    }
    else if(S(D,TEXT("format"))==TEXT("usd"))return VTSettings(D,Issues);
    return Out;
}
}

