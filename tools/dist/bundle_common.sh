# Shared by make_bundle.sh and make_bundle_macos.sh (sourced, run from the repository root with $root set).
# A bundle ships the committed tree only: files come from HEAD, never from the working directory, and the finished
# archive is checked for files that are not in the commit and for paths of the build machine.

# The binaries are built from the working tree, so it must match HEAD (BUNDLE_ALLOW_DIRTY=1 for a local trial).
bundle_require_clean_tree() {
    [ "${BUNDLE_ALLOW_DIRTY:-0}" = 1 ] && return 0
    if ! git diff --quiet HEAD -- || [ -n "$(git ls-files --others --exclude-standard)" ]; then
        echo "bundle: the working tree differs from HEAD; commit first (or BUNDLE_ALLOW_DIRTY=1 for a trial)" >&2
        exit 1
    fi
}

# bundle_stage DEST PATHSPEC...: the committed files that match, at their repository paths under DEST.
bundle_stage() {
    local dest=$1 f
    shift
    git ls-files -z -- "$@" | while IFS= read -r -d '' f; do
        install -d "$dest/$(dirname "$f")"
        git show "HEAD:$f" > "$dest/$f"
        chmod 644 "$dest/$f"
    done
}

# bundle_stage_flat DEST DIRECTORY: the committed files of DIRECTORY, directly in DEST.
bundle_stage_flat() {
    local dest=$1 dir=$2
    install -d "$dest"
    git archive --format=tar HEAD "$dir" | tar -x -C "$dest" --strip-components="$(tr -cd / <<< "$dir/" | wc -c)"
}

# bundle_stage_recipe RECIPE DEST: the committed data/recipe.json.gz; another recipe (RECIPE=FILE) only for a local
# trial (BUNDLE_ALLOW_DIRTY=1), never for a release.
bundle_stage_recipe() {
    local recipe=$1 dest=$2
    if [ -z "${RECIPE:-}" ]; then
        git show HEAD:data/recipe.json.gz > "$dest"
    elif [ "${BUNDLE_ALLOW_DIRTY:-0}" = 1 ]; then
        install -m 644 "$recipe" "$dest"
    else
        echo "bundle: RECIPE=$recipe is not the committed recipe; set BUNDLE_ALLOW_DIRTY=1 for a trial" >&2
        exit 1
    fi
    chmod 644 "$dest"
}

# bundle_stage_ffmpeg_source PREFIX DEST: the source of the bundled FFmpeg libraries and the script that configures
# and builds them, so that the bundle carries the corresponding source of its LGPL libraries. PREFIX must hold the
# pinned version of tools/build_ffmpeg.sh; its tarball (downloaded again when it is gone) is checked by SHA-256.
bundle_stage_ffmpeg_source() {
    local prefix=$1 dest=$2 version sum tarball
    version="$(sed -n 's/^version=//p' tools/build_ffmpeg.sh)"
    sum="$(sed -n 's/^sha256=//p' tools/build_ffmpeg.sh)"
    if ! grep -q "FFMPEG_VERSION \"$version-sacred-native-minimal\"" "$prefix/include/libavutil/ffversion.h"; then
        echo "bundle: $prefix is not the pinned FFmpeg $version of tools/build_ffmpeg.sh" >&2
        exit 1
    fi

    tarball="$root/build/ffmpeg/dl/ffmpeg-$version.tar.xz"
    if [ ! -s "$tarball" ]; then
        mkdir -p "$(dirname "$tarball")"
        curl -fsSL -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"
    fi

    # As tools/build_ffmpeg.sh checks it: the sha256sum of macOS knows --check but not --status.
    if command -v sha256sum >/dev/null; then
        printf '%s  %s\n' "$sum" "$tarball" | sha256sum --check - >/dev/null
    else
        printf '%s  %s\n' "$sum" "$tarball" | shasum -a 256 --check - >/dev/null
    fi || { echo "bundle: checksum mismatch: $tarball" >&2; exit 1; }

    install -d "$dest"
    install -m 644 "$tarball" "$dest/ffmpeg-$version.tar.xz"
    git show HEAD:tools/build_ffmpeg.sh > "$dest/ffmpeg-build.sh"
    chmod 644 "$dest/ffmpeg-build.sh"
}

# bundle_source_name FOUND REAL: where a bundled library came from, for lib/BUNDLED_LIBRARIES.txt: the path it was
# found at, relative to the repository, with the file name it resolves to (REAL). The directory is not resolved:
# build/ may be a link to a directory elsewhere, whose absolute path must not get into the bundle.
bundle_source_name() {
    local found
    found="$(dirname "$1")/$(basename "$2")"
    printf '%s\n' "${found#"$root"/}"
}

# bundle_verify ARCHIVE TOP [RESOURCES]: the menu pictures and the translator sources in the archive (a .tar.gz, or
# a .zip of the macOS app) are exactly the committed ones, and no file mentions the repository path or the home
# directory of the build machine. TOP is the archive's top directory, RESOURCES the directory below it that holds
# menu-backgrounds/ and source/ (TOP itself unless given: "Sacred Native.app/Contents/Resources" for the app).
bundle_verify() {
    local archive=$1 name=$2 check
    local resources="$name${3:+/$3}"
    check="$root/build/bundle-check"
    rm -rf "$check" && mkdir -p "$check"
    case "$archive" in
        *.zip) ditto -x -k "$archive" "$check" ;;
        *) tar -C "$check" -xzf "$archive" ;;
    esac
    local bad=0 f
    (cd "$check/$resources/menu-backgrounds" && find . -type f | sed 's|^\./||' | sort) > "$check/pictures.found"
    git ls-files -- data/menu-backgrounds | sed 's|^data/menu-backgrounds/||' | sort > "$check/pictures.committed"
    cmp -s "$check/pictures.found" "$check/pictures.committed" ||
        { echo "bundle: menu-backgrounds/ differs from the committed files" >&2; bad=1; }
    (cd "$check/$resources/source" && find . -type f ! -name recipe.json.gz | sed 's|^\./||' | sort) \
        > "$check/source.found"
    while IFS= read -r f; do
        git ls-files --error-unmatch -- "$f" >/dev/null 2>&1 ||
            { echo "bundle: source/$f is not a committed file" >&2; bad=1; }
    done < "$check/source.found"

    if [ "${BUNDLE_ALLOW_DIRTY:-0}" != 1 ]; then
        git show HEAD:data/recipe.json.gz > "$check/recipe.committed"
        cmp -s "$check/$resources/source/recipe.json.gz" "$check/recipe.committed" ||
            { echo "bundle: source/recipe.json.gz is not the committed recipe" >&2; bad=1; }
    fi

    local leaks
    leaks=$(LC_ALL=C grep -a -r -l -F -e "$root" -e "$HOME/" "$check/$name" | sed "s|^$check/||" || true)
    if [ -n "$leaks" ]; then
        printf 'bundle: build-machine paths in:\n%s\n' "$leaks" >&2
        bad=1
    fi

    # The macOS app as the player unpacks it: its signature still holds (nothing in the archive but the app).
    if [ "${name%.app}" != "$name" ]; then
        codesign --verify --deep --strict "$check/$name" ||
            { echo "bundle: the unpacked app's signature does not verify" >&2; bad=1; }
        [ "$(ls -A "$check" | grep -v -e '\.found$' -e '\.committed$')" = "$name" ] ||
            { echo "bundle: the archive holds more than $name" >&2; bad=1; }
    fi

    rm -rf "$check"
    [ "$bad" = 0 ] || exit 1
    echo "BUNDLE_VERIFIED $archive"
}
