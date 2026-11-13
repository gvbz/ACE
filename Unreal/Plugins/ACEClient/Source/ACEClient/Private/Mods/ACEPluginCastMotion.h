#pragma once
#include "CoreMinimal.h"

namespace ACEPluginCastMotion
{
    // Discard a tap blocked by UI/airborne movement instead of replaying it later.
    // This bounds request freshness, not how long a backward key is held.
    constexpr double TapRequestSeconds = 0.12;
    // VT MySpell.IsInstantCast eligibility. Ordinary item/other buffs retain
    // their casting animation.
    inline bool Instant(uint32 School,uint32 Power,uint32 Flags,double Duration,uint32 Category=0)
    {
        return Power<50||((Flags&8)&&!(Flags&0x2000)&&Duration>=60&&(School==2||School==4))
            ||(Category>=243&&Category<=249)||Category==639;
    }
    inline bool FastBuff(uint32 School,uint32 Power,uint32 Flags,double Duration,uint32 Category=0)
    {
        if(School<1||School>5||School==1||School==5)return false;
        return Instant(School,Power,Flags,Duration,Category);
    }
    inline bool Jiggle(uint32 School,uint32 Power,uint32 Flags,double Duration,uint32 Category,bool Item)
    {return Item?!Instant(School,Power,Flags,Duration,Category):!Instant(School,Power,Flags,Duration,Category)||(School!=1&&School!=5);}
    inline float JiggleStrafe(double Elapsed){return (int32(FMath::Max(0.,Elapsed)/.131)&1)?-1.f:1.f;}
}
