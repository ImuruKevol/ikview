#!/bin/sh
# Install a stable app bundle plus the user-local terminal launcher.
set -eu
umask 022
task_resource_dir=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd)
task_app=""
task_prefix="$HOME/.local"
task_destination="$HOME/Applications/IKView.app"
task_remove_cli=0
fail() { printf 'IKView: %s\n' "$*" >&2; exit 1; }
while [ "$#" -gt 0 ]; do
    case "$1" in
        --app|--prefix|--destination)
            [ "$#" -ge 2 ] || fail "Missing value for $1"
            case "$1" in
                --app) task_app=$2;; --prefix) task_prefix=$2;; --destination) task_destination=$2;;
            esac
            shift 2;;
        --remove-cli) task_remove_cli=1; shift;;
        --help|-h)
            printf '%s\n' 'Install IKView and its native Finder extension without sudo.' \
                'Options: --app SOURCE.app --destination TARGET.app --prefix PREFIX' \
                'Defaults: ~/Applications/IKView.app and ~/.local/bin/ikview' \
                '--remove-cli removes only the managed terminal launcher, keeping the app and Finder extension.'
            exit 0;;
        *) fail "Unknown option: $1";;
    esac
done
[ "$(uname -s)" = Darwin ] || fail 'This installer requires macOS.'
case "$task_prefix" in /*) ;; *) fail '--prefix must be an absolute path.';; esac
case "$task_destination" in /*.app) ;; *) fail '--destination must be an absolute .app path.';; esac
# The configuration is one literal path per line; image arguments have no such restriction.
case "$task_destination$task_prefix" in *'
'*) fail 'Installation paths cannot contain newlines.';; esac
task_launcher="$task_prefix/bin/ikview"
task_config="$task_prefix/share/ikview/app-path"
if [ -L "$task_launcher" ] || [ -L "$task_config" ]; then fail 'Refusing to replace a symlink at a managed file path.'; fi
if [ -e "$task_launcher" ]; then
    [ "$(/usr/bin/sed -n '2p' "$task_launcher")" = '# IKView managed terminal launcher v1' ] || fail "Another command already exists at $task_launcher"
fi
if [ -e "$task_config" ] && [ ! -f "$task_launcher" ]; then fail 'Existing app-path has no managed launcher; preserve it before installing.'; fi
if [ "$task_remove_cli" -eq 1 ]; then
    if [ -f "$task_launcher" ]; then rm -f -- "$task_launcher" "$task_config"; fi
    printf '%s\n' 'Removed the managed terminal command. The app and Finder extension remain installed.'
    exit 0
fi
if [ -z "$task_app" ]; then task_app=$(CDPATH= cd -- "$task_resource_dir/../../.." && pwd -P); fi
[ -x "$task_app/Contents/MacOS/ikview" ] || fail "App executable not found in $task_app"
task_app=$(CDPATH= cd -- "$task_app" && pwd -P)
[ "$(/usr/bin/plutil -extract CFBundleIdentifier raw -o - "$task_app/Contents/Info.plist")" = io.ikview.desktop ] || fail 'Source is not an IKView app bundle.'
/usr/bin/codesign --verify --deep --strict "$task_app" || fail 'Source app signature could not be verified.'
mkdir -p -- "$(dirname -- "$task_destination")" "$task_prefix/bin" "$task_prefix/share/ikview"
task_parent=$(CDPATH= cd -- "$(dirname -- "$task_destination")" && pwd -P)
task_destination="$task_parent/$(basename -- "$task_destination")"
[ ! -L "$task_destination" ] || fail 'The destination app cannot be a symlink.'
task_stage=$(mktemp -d "$task_parent/.ikview-install.XXXXXX")
task_installed=0
task_committed=0
task_config_published=0
task_launcher_published=0
restore_file() {
    task_restore_path=$1
    task_restore_backup=$2
    if [ -f "$task_restore_backup" ]; then
        task_restore_tmp=$(mktemp "$(dirname -- "$task_restore_path")/.ikview-restore.XXXXXX") || return 1
        if cp -p -- "$task_restore_backup" "$task_restore_tmp" && mv -f -- "$task_restore_tmp" "$task_restore_path"; then return 0; fi
        rm -f -- "$task_restore_tmp"
        return 1
    fi
    rm -f -- "$task_restore_path"
}
cleanup() {
    task_result=$?
    trap - EXIT HUP INT TERM
    task_recovery_failed=0
    if [ "$task_committed" -eq 0 ]; then
        if [ "$task_launcher_published" -eq 1 ]; then restore_file "$task_launcher" "$task_stage/previous-launcher" || task_recovery_failed=1; fi
        if [ "$task_config_published" -eq 1 ]; then restore_file "$task_config" "$task_stage/previous-config" || task_recovery_failed=1; fi
    fi
    if [ "$task_committed" -eq 0 ] && [ "$task_installed" -eq 1 ]; then
        if [ -d "$task_stage/previous.app" ]; then
            if [ -e "$task_destination" ]; then mv -- "$task_destination" "$task_stage/failed.app" || task_recovery_failed=1; fi
            if [ ! -e "$task_destination" ]; then mv -- "$task_stage/previous.app" "$task_destination" || task_recovery_failed=1; fi
        elif [ ! -d "$task_stage/IKView.app" ] && [ -e "$task_destination" ]; then
            mv -- "$task_destination" "$task_stage/failed.app" || task_recovery_failed=1
        fi
    fi
    if [ -n "${task_launcher_tmp-}" ]; then rm -f -- "$task_launcher_tmp"; fi
    if [ -n "${task_config_tmp-}" ]; then rm -f -- "$task_config_tmp"; fi
    if [ "$task_recovery_failed" -eq 1 ] || { [ -d "$task_stage/previous.app" ] && [ "$task_committed" -eq 0 ]; }; then
        printf 'Recovery files retained at %s\n' "$task_stage" >&2
    else rm -rf -- "$task_stage"; fi
    exit "$task_result"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM
if [ "$task_app" != "$task_destination" ]; then
    if [ -e "$task_destination" ]; then
        [ "$(/usr/bin/plutil -extract CFBundleIdentifier raw -o - "$task_destination/Contents/Info.plist")" = io.ikview.desktop ] || fail 'Destination belongs to another app.'
        if /usr/sbin/lsof -t -- "$task_destination/Contents/MacOS/ikview" >/dev/null 2>&1; then fail 'Quit the installed IKView before updating it.'; fi
    fi
    /usr/bin/ditto "$task_app" "$task_stage/IKView.app"
    /usr/bin/codesign --verify --deep --strict "$task_stage/IKView.app"
    task_installed=1
    if [ -e "$task_destination" ]; then mv -- "$task_destination" "$task_stage/previous.app"; fi
    if ! mv -- "$task_stage/IKView.app" "$task_destination"; then
        fail 'Could not publish the app.'
    fi
fi
"$task_destination/Contents/MacOS/ikview" --register
# Write each file atomically, after registration succeeds.
if [ -f "$task_launcher" ]; then cp -p -- "$task_launcher" "$task_stage/previous-launcher"; fi
if [ -f "$task_config" ]; then cp -p -- "$task_config" "$task_stage/previous-config"; fi
task_launcher_tmp=$(mktemp "$task_prefix/bin/.ikview.XXXXXX")
task_config_tmp=$(mktemp "$task_prefix/share/ikview/.app-path.XXXXXX")
cp -- "$task_resource_dir/ikview" "$task_launcher_tmp"
chmod 755 "$task_launcher_tmp"
printf '%s\n' "$task_destination" > "$task_config_tmp"
chmod 644 "$task_config_tmp"
task_config_published=1
mv -f -- "$task_config_tmp" "$task_config"
task_launcher_published=1
mv -f -- "$task_launcher_tmp" "$task_launcher"
task_committed=1
printf 'Installed app: %s\nTerminal command: %s\n' "$task_destination" "$task_launcher"
if [ -x "$task_destination/Contents/PlugIns/IKViewFinder.appex/Contents/MacOS/IKViewFinder" ]; then
    if /bin/sh "$task_resource_dir/finder-integration" --app "$task_destination" --enable; then
        printf '%s\n' 'Finder: IKView로 열기 / IKView로 폴더 열기 appear directly in the context menu after Finder loads the extension.'
    else
        printf '%s\n' 'The app and terminal command are installed; Finder activation is pending.' \
            'Run ikview --finder-status or ikview --finder-enable to inspect/retry.' >&2
    fi
else
    printf '%s\n' 'This app does not bundle the native Finder extension.'
fi
case ":$PATH:" in *":$task_prefix/bin:"*) ;; *)
    printf 'Add %s/bin to your shell PATH. For the default prefix:\n' "$task_prefix"
    printf '%s\n' 'export PATH="$HOME/.local/bin:$PATH"';;
esac
