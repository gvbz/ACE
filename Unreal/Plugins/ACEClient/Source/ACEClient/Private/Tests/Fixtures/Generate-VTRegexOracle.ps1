# Regenerate the native regex fixtures with Microsoft's .NET regex engine.
# No runtime dependency on PowerShell/.NET is introduced into the client.
$ErrorActionPreference='Stop'
$patterns=@(
 '^(a)?(?(1)b|c)$', '^(a?)(?(1)b|c)$', '^(?<x>a)?(?(x)b|c)$',
 '^(a|ab)?(?(1)c|ab)$', '^(?:(?<x>a)b|a)(?(x)c|b)$',
 '^(?(?=a)a|b)$', '^(?(?!a)b|a)$', '^(?(?=(a))\1|b)$',
 '^(?(a)a|b)$', '^(?i)(?(a)A|B)$',
 '^(?<o>a)+(?<-o>b)+(?(o)(?!))$',
 '^(?<o>a)+(?<between-o>b)+(?(o)(?!))$',
 '^(?<o>a)(?<o>b)(?<-o>c)\k<o>$',
 '^(?<o>a)(?:(?<-o>b)c|b)\k<o>$',
 '^(?:(?<o>\()|(?<-o>\))|[^()])*(?(o)(?!))$',
 '^(?<o>a)?(?<-o>b)$', '^(?<o>a)(?<gap-o>b)$',
 '^(?<o>a)(?<gap-o>.)b$',
 '^(?<outer>(?<inner>a))(?(1)b|c)\k<outer>$',
 '^(?<named>a)(b)(?(1)c|d)\2$',
 '^(?n)(?<x>a)(b)(?(x)c|d)\1$',
 '(?x) ^ (?<o>a)+ # pushes\n (?<-o>b)+ (?(o)(?!)) $'.Replace('\n',"`n"),
 '^(?<o>a)*?(?<-o>b)*(?(o)(?!))$',
 '^(?<o>a){1,3}(?<-o>b){1,3}(?(o)(?!))$',
 '^(?<o>a?){2}(?<-o>b){2}(?(o)(?!))$',
 '^(?<o>)(?<-o>)*$', '^(?<o>a)(?>ab|a)(?<-o>b)$',
 '^(?<o>a)(?=b)(?<-o>b)$', '^(?<o>a)(?!c)(?<-o>b)$',
 '^a(?<=(?<x>a))(?(x)b|c)$', '^a(?(?<=a)b|c)$',
 '^a(?(?<!b)b|c)$', '^ab(?<=(?<x>a)b)(?(x)c|d)$',
 '^(?<o>a)(?=(?<-o>b))b(?(o)c|d)$',
 '^(?<o>a)(?!(?<-o>b)c)b(?(o)c|d)$',
 '^(?<2>a)(b)(?(2)c|d)\1$',
 '^(?<o>a)(?<-o>b)(?(o)c|d)$',
 '^(?<o>a)(?<o>b)(?<-o>c)(?(o)\k<o>|d)$',
 '^(?<o>\p{L})+(?<-o>\d)+(?(o)(?!))$',
 '^(?i)(?<o>a)(?<-o>b)(?(o)c|d)$',
 '^(?<o>a)(?<-o>b)\10$',
 '^(?<o>a)(?<o>b)(?(o)c|d)$',
 '^(?<o>a)(?<gap-o>(?<inner>b))$',
 '^(?(?=(?<x>a+))\k<x>a|b)$',
 '^(?<o>a)(?<-o>(?<o>b))(?(o)c|d)$',
 '^a(?<=(?<o>a)(?<-o>))(?(o)b|c)$',
 '^a(?<=(?<-o>)(?<o>a))(?(o)b|c)$',
 '^(?<o>a)(?<-o>b){0}(?(o)c|d)$',
 '^(?<o>a)(?<-o>b)(?(o)(?(o)c|d)|e)$',
 '^(?(?=((a)))a|b)$', '^(?(?=(?:a(b)(c)))abc|d)$',
 '^(?(?=a)a(b)|c)$', '^(?(a)a(b)|c)$',
 '^(?<x>a)(?<X>b)(?(x)c|d)(?(X)e|f)$',
 '^(?<x>a)(?<X>b)(?<-x>c)(?(x)d|e)(?(X)f|g)$',
 '^aaab(?<=(?<o>a)+(?<-o>b))(?(o)c|d)$',
 '^(?<o>a)(?<-o>b)\G$',
 '(?(?<=a)b|c)', '^(?<o>a){0,2}?(?<-o>b){0,2}?(?(o)(?!))$',
 '^(?<o>a)(?<o>b)\k<o>$', '^(?<4>a)(b)\k<4>\1$',
 '^(?<o>a)(?:(?<o>b)c|b)\k<o>$'
)
$texts=@('', 'a','b','c','ab','ac','bc','abc','abd','abe','aba','abca','abb','aab','aaabbb','aabb','aaabb','aabbb','abcd','abcba','abab','aaab','bbb','ba','(a(b)c)','(a(b)c','a)b','()','(())','(()))',([string][char]0xe9+'1'),'abc123','AB','AbD','ab'+[char]8)
$texts+=@(('ab'+[char]8),'abce','abcef','abcef','aaabc','aaabd','abcd','acac','abcde')
$fixtures=[System.Collections.Generic.List[object]]::new()
foreach($pattern in $patterns){
 $regex=[regex]::new($pattern,[System.Text.RegularExpressions.RegexOptions]::None,[timespan]::FromSeconds(1))
 foreach($text in $texts){
  $match=$regex.Match($text)
  $captures=[System.Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
  if($match.Success){
   foreach($number in $regex.GetGroupNumbers()){if($match.Groups[$number].Success){$captures[[string]$number]=$match.Groups[$number].Value}}
   foreach($name in $regex.GetGroupNames()){if($match.Groups[$name].Success){$captures[$name]=$match.Groups[$name].Value}}
  }
  # Store entries as an array: Unreal's ordinary JSON object keys fold case.
  $entries=@($captures.GetEnumerator()|ForEach-Object {[ordered]@{name=$_.Key;value=$_.Value}})
  $fixtures.Add([ordered]@{pattern=$pattern;text=$text;matches=$match.Success;captures=$entries})
 }
}
$output=[ordered]@{engine=[System.Runtime.InteropServices.RuntimeInformation]::FrameworkDescription;cases=$fixtures}
$output|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $PSScriptRoot 'VTRegexDotNet.json') -Encoding utf8
Write-Output "Generated $($fixtures.Count) .NET regex cases."
