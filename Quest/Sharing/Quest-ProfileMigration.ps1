#requires -Version 5.1
# The installation ID deliberately stays stable: Android owns the encrypted
# login key and app data under that ID. Never uninstall as part of a rename.
function Move-QuestLegacyProfile {
    $probe = "if [ -d files/ACEViewer ]; then echo legacy; fi"
    $state = Invoke-Adb -Arguments ($deviceArgs + @('shell', 'run-as', $package, 'sh', '-c', "'$probe'")) | Out-String
    if ($state.Trim() -ne 'legacy') { return }
    Invoke-Adb -Arguments ($deviceArgs + @('shell', 'am', 'force-stop', $package)) | Out-Host
    # Move the full profile atomically on a normal upgrade. If a new profile
    # already exists, retain its files and fill only missing data/settings.
    $migration = @'
set -eu;
old=files/ACEViewer;
new=files/ACUnreal;
if [ ! -e "$new" ]; then
    mv "$old" "$new";
else
    mkdir -p "$new/Saved";
    for section in Config Login Journal SaveGames DAT; do
        source="$old/Saved/$section";
        target="$new/Saved/$section";
        [ -d "$source" ] || continue;
        if [ ! -e "$target" ]; then
            mv "$source" "$target";
        elif [ "$section" = DAT ]; then
            for name in client_portal.dat client_cell_1.dat client_local_English.dat client_highres.dat; do
                if [ -f "$source/$name" ] && [ ! -e "$target/$name" ]; then mv "$source/$name" "$target/$name"; fi;
            done;
        else
            cp -R -n "$source/." "$target/";
        fi;
    done;
    atlas=Saved/ClientPlugins/Waypoint/locations.json;
    if [ -f "$old/$atlas" ] && [ ! -e "$new/$atlas" ]; then
        mkdir -p "$new/Saved/ClientPlugins/Waypoint";
        mv -n "$old/$atlas" "$new/$atlas";
    fi;
fi;
'@
    $migration = $migration.Replace("`r", '').Replace("`n", ' ')
    Invoke-Adb -Arguments ($deviceArgs + @('shell', 'run-as', $package, 'sh', '-c', "'$migration'")) | Out-Host
    Write-Host 'Existing game data, login and settings migrated to the AC:Unreal shared runtime folder.'
}
