#!/usr/bin/env bash
# Generic helper script for maintaining esp-devel/* (espressif/qemu) and
# qemu-devel/* (upstream QEMU) patch stacks using Git's triangular workflow:
#   - Base branches (esp-devel/000-base, qemu-devel/000-base) pull from
#     espressif/esp-develop and upstream/master, and push to origin.
#   - Stack branches (esp-devel/*, qemu-devel/*) track and push to origin.
#
# Remotes expected:
#   origin     - your fork (push target, remote.pushDefault=origin)
#   espressif  - https://github.com/espressif/qemu.git
#   upstream   - https://gitlab.com/qemu-project/qemu.git

set -euo pipefail

ESPRESSIF_URL="${ESPRESSIF_URL:-https://github.com/espressif/qemu.git}"
UPSTREAM_URL="${UPSTREAM_URL:-https://gitlab.com/qemu-project/qemu.git}"
ESP_BASE="${ESP_BASE:-esp-devel/000-base}"
ESP_UPSTREAM_BRANCH="${ESP_UPSTREAM_BRANCH:-esp-develop}"
QEMU_BASE="${QEMU_BASE:-qemu-devel/000-base}"
ESP_PREFIX="${ESP_PREFIX:-esp-devel/}"
QEMU_PREFIX="${QEMU_PREFIX:-qemu-devel/}"

list_esp_branches() {
    git for-each-ref --format='%(refname:short)' --sort=refname \
        "refs/heads/${ESP_PREFIX}*" | grep -v "^${ESP_BASE}$" || true
}

list_qemu_branches() {
    git for-each-ref --format='%(refname:short)' --sort=refname \
        "refs/heads/${QEMU_PREFIX}*" | grep -v "^${QEMU_BASE}$" || true
}

usage() {
    cat <<'EOF'
Usage: scripts/qemu-stack.sh <command> [args]

Commands:
  setup                          Configure remotes, triangular pushDefault=origin, and tracking
  status                         Show remotes and auto-discovered stack branches
  rebase-esp                     Fetch espressif/esp-develop and rebase all esp-devel/* (+ wip)
  rebase-upstream                Fetch upstream/master (--depth=1) and rebase all qemu-devel/*
  checkpatch [esp|upstream|all]  Run scripts/checkpatch.pl on stack branches (default: all)
  push [esp|upstream|all]        Push branches to origin with --force-with-lease (default: all)
  format-patch <branch> [dir]    Export git format-patch series + print maintainers
EOF
}

require_clean() {
    if ! git diff-index --quiet HEAD --; then
        echo "error: working tree is dirty; commit or stash first" >&2
        exit 1
    fi
}

cmd_setup() {
    if ! git remote get-url espressif >/dev/null 2>&1; then
        git remote add espressif "$ESPRESSIF_URL"
    fi
    if ! git config --get remote.espressif.fetch >/dev/null 2>&1; then
        git config remote.espressif.fetch "+refs/heads/*:refs/remotes/espressif/*"
    fi
    if ! git remote get-url upstream >/dev/null 2>&1; then
        git remote add upstream "$UPSTREAM_URL"
    fi
    if ! git config --get remote.upstream.fetch >/dev/null 2>&1; then
        git config remote.upstream.fetch "+refs/heads/*:refs/remotes/upstream/*"
    fi

    # Triangular workflow: base branches pull from upstream/espressif, push to origin.
    git config remote.pushDefault origin

    git fetch espressif "$ESP_UPSTREAM_BRANCH"
    git fetch --depth=1 upstream master

    if git show-ref --verify --quiet "refs/heads/$ESP_BASE"; then
        git branch --set-upstream-to="espressif/$ESP_UPSTREAM_BRANCH" "$ESP_BASE"
    else
        git branch "$ESP_BASE" "espressif/$ESP_UPSTREAM_BRANCH"
    fi
    git config "branch.${ESP_BASE}.pushRemote" origin

    if git show-ref --verify --quiet "refs/heads/$QEMU_BASE"; then
        git branch --set-upstream-to="upstream/master" "$QEMU_BASE"
    else
        git branch "$QEMU_BASE" "upstream/master"
    fi
    git config "branch.${QEMU_BASE}.pushRemote" origin

    local br
    while IFS= read -r br; do
        [ -n "$br" ] || continue
        if git show-ref --verify --quiet "refs/remotes/origin/$br"; then
            git branch --set-upstream-to="origin/$br" "$br"
        fi
    done < <(list_esp_branches; list_qemu_branches)

    echo "Setup complete (remote.pushDefault=origin, bases track espressif & upstream)."
}

cmd_status() {
    echo "=== Remotes ==="
    git remote -v
    echo ""
    echo "=== esp-devel stack (base: $ESP_BASE) ==="
    list_esp_branches
    echo ""
    echo "=== qemu-devel branches (base: $QEMU_BASE) ==="
    list_qemu_branches
    echo ""
    echo "=== All local branches ==="
    git branch -vv
}

cmd_rebase_esp() {
    require_clean
    local orig_branch old_base old_tip esp_tip
    local -a branches
    mapfile -t branches < <(list_esp_branches)
    if [ "${#branches[@]}" -eq 0 ]; then
        echo "No ${ESP_PREFIX}* branches found."
        return 0
    fi

    esp_tip="${branches[${#branches[@]}-1]}"
    orig_branch=$(git symbolic-ref --short HEAD 2>/dev/null || git rev-parse HEAD)
    old_base=$(git rev-parse "$ESP_BASE")
    old_tip=$(git rev-parse "$esp_tip")

    echo "Fetching espressif/$ESP_UPSTREAM_BRANCH..."
    git fetch espressif "$ESP_UPSTREAM_BRANCH"
    git branch -f "$ESP_BASE" "espressif/$ESP_UPSTREAM_BRANCH"

    if [ "$(git rev-parse "$ESP_BASE")" = "$old_base" ]; then
        echo "$ESP_BASE is already up to date ($old_base)."
        return 0
    fi

    echo "Rebasing ${ESP_PREFIX}* stack (tip: $esp_tip) onto $ESP_BASE with --update-refs..."
    git checkout "$esp_tip"
    git rebase --update-refs --onto "$ESP_BASE" "$old_base"

    if git show-ref --verify --quiet refs/heads/wip; then
        echo "Rebasing wip onto $esp_tip..."
        git checkout wip
        git rebase --onto "$esp_tip" "$old_tip"
    fi

    git checkout "$orig_branch"
    echo "Done. Review with 'git branch -v' and run 'scripts/qemu-stack.sh checkpatch esp'."
}

cmd_rebase_upstream() {
    require_clean
    local orig_branch old_base new_base
    local -a branches
    mapfile -t branches < <(list_qemu_branches)
    if [ "${#branches[@]}" -eq 0 ]; then
        echo "No ${QEMU_PREFIX}* branches found."
        return 0
    fi

    orig_branch=$(git symbolic-ref --short HEAD 2>/dev/null || git rev-parse HEAD)
    old_base=$(git rev-parse "$QEMU_BASE")

    echo "Fetching upstream/master (--depth=1)..."
    git fetch --depth=1 upstream master
    new_base=$(git rev-parse upstream/master)

    if [ "$new_base" = "$old_base" ]; then
        echo "$QEMU_BASE is already up to date ($old_base)."
        return 0
    fi

    for br in "${branches[@]}"; do
        echo "=== Rebasing $br onto $new_base ==="
        git checkout "$br"
        git rebase --onto "$new_base" "$old_base"
    done

    git branch -f "$QEMU_BASE" "$new_base"
    git checkout "$orig_branch"
    echo "Done. All ${QEMU_PREFIX}* branches rebased onto $new_base."
}

cmd_checkpatch() {
    local scope="${1:-all}"
    local rc=0
    local -a branches

    if [ "$scope" = "esp" ] || [ "$scope" = "all" ]; then
        mapfile -t branches < <(list_esp_branches)
        if [ "${#branches[@]}" -gt 0 ]; then
            local esp_tip="${branches[${#branches[@]}-1]}"
            echo "=== checkpatch: $ESP_BASE..$esp_tip ==="
            perl scripts/checkpatch.pl --branch "$ESP_BASE..$esp_tip" || rc=1
        fi
    fi

    if [ "$scope" = "upstream" ] || [ "$scope" = "all" ]; then
        mapfile -t branches < <(list_qemu_branches)
        for br in "${branches[@]}"; do
            echo "=== checkpatch: $QEMU_BASE..$br ==="
            perl scripts/checkpatch.pl --branch "$QEMU_BASE..$br" || rc=1
        done
    fi

    return "$rc"
}

cmd_push() {
    local scope="${1:-all}"
    local -a branches work_refs=() base_refs=()

    if [ "$scope" = "esp" ] || [ "$scope" = "all" ]; then
        mapfile -t branches < <(list_esp_branches)
        base_refs+=("$ESP_BASE")
        work_refs+=("${branches[@]}")
        if git show-ref --verify --quiet refs/heads/wip; then
            work_refs+=(wip)
        fi
    fi

    if [ "$scope" = "upstream" ] || [ "$scope" = "all" ]; then
        mapfile -t branches < <(list_qemu_branches)
        base_refs+=("$QEMU_BASE")
        work_refs+=("${branches[@]}")
    fi

    if [ "${#base_refs[@]}" -gt 0 ]; then
        echo "Pushing base branches to origin (keeping upstream tracking): ${base_refs[*]}"
        git push --force-with-lease origin "${base_refs[@]}"
    fi
    if [ "${#work_refs[@]}" -gt 0 ]; then
        echo "Pushing stack branches to origin: ${work_refs[*]}"
        git push -u --force-with-lease origin "${work_refs[@]}"
    fi
}

cmd_format_patch() {
    local br="${1:-}"
    local out_dir="${2:-/tmp/qemu-patches-${br##*/}}"
    if [ -z "$br" ]; then
        echo "error: specify a branch (e.g. ${QEMU_PREFIX}100-xtensa-interrupts)" >&2
        exit 1
    fi
    local base="$QEMU_BASE"
    if [[ "$br" == "${ESP_PREFIX}"* ]]; then
        base="$ESP_BASE"
    fi

    mkdir -p "$out_dir"
    rm -f "$out_dir"/*.patch
    git format-patch --cover-letter -o "$out_dir" "$base..$br"
    echo ""
    echo "=== Maintainers (scripts/get_maintainer.pl) ==="
    perl scripts/get_maintainer.pl --noroles --nogit-fallback "$out_dir"/0001-*.patch || true
    echo ""
    echo "Patches written to: $out_dir"
}

main() {
    local cmd="${1:-help}"
    shift || true
    case "$cmd" in
        setup)           cmd_setup "$@" ;;
        status)          cmd_status "$@" ;;
        rebase-esp)      cmd_rebase_esp "$@" ;;
        rebase-upstream) cmd_rebase_upstream "$@" ;;
        checkpatch)      cmd_checkpatch "$@" ;;
        push)            cmd_push "$@" ;;
        format-patch)    cmd_format_patch "$@" ;;
        -h|--help|help)  usage ;;
        *)
            echo "Unknown command: $cmd" >&2
            usage >&2
            exit 1
            ;;
    esac
}

main "$@"
