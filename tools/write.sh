#!/bin/bash
# UnoDOS Disk Image Writer - Linux TUI
# Writes floppy or HD images to any target drive
#
# Usage: sudo ./tools/write.sh [options]
#   -i, --image PATH    Image file path (skip image selection)
#   -d, --device PATH   Device path e.g. /dev/sdc (skip drive selection)
#   -v, --verify        Verify after writing
#   -h, --help          Show usage
#
# No arguments = full interactive mode (image selection + drive selection)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

# CLI arguments
ARG_IMAGE=""
ARG_DEVICE=""
ARG_VERIFY=0

# State
declare -a IMG_PATHS=()
declare -a IMG_NAMES=()
declare -a IMG_SIZES=()
declare -a IMG_VERSIONS=()
declare -a IMG_BUILDS=()
declare -a IMG_DESCS=()
IMG_COUNT=0

declare -a DRV_NAMES=()
declare -a DRV_PATHS=()
declare -a DRV_SIZES=()
declare -a DRV_SIZE_BYTES=()
declare -a DRV_MODELS=()
declare -a DRV_TRANS=()
declare -a DRV_REMOVABLE=()
declare -a DRV_TYPES=()
DRV_COUNT=0

SELECTED_IMAGE=""
SELECTED_IMAGE_IDX=-1
SELECTED_DEVICE=""
SELECTED_DEVICE_IDX=-1

# Terminal dimensions
TERM_COLS=80
TERM_ROWS=24

# Color codes (tput setaf)
C_BLACK=0; C_RED=1; C_GREEN=2; C_YELLOW=3; C_BLUE=4; C_MAGENTA=5; C_CYAN=6; C_WHITE=7
C_GRAY=8; C_BRIGHT_RED=9; C_BRIGHT_GREEN=10; C_BRIGHT_YELLOW=11

# ─── TUI Helper Functions ────────────────────────────────────────────────────

cleanup() {
    tput cnorm 2>/dev/null   # Show cursor
    tput rmcup 2>/dev/null   # Restore screen
    tput sgr0 2>/dev/null    # Reset colors
    stty echo 2>/dev/null    # Restore echo
}

die() {
    cleanup
    echo "ERROR: $*" >&2
    exit 1
}

init_tui() {
    trap cleanup EXIT
    trap 'cleanup; exit 130' INT TERM
    TERM_COLS=$(tput cols)
    TERM_ROWS=$(tput lines)
    tput smcup              # Alternate screen buffer
    tput civis              # Hide cursor
    tput clear              # Clear screen
    stty -echo              # Disable echo
}

write_at() {
    local x=$1 y=$2
    shift 2
    local text="$1"
    local fg="${2:-}"
    local bg="${3:-}"
    tput cup "$y" "$x"
    [[ -n "$fg" ]] && tput setaf "$fg"
    [[ -n "$bg" ]] && tput setab "$bg"
    printf '%s' "$text"
    tput sgr0
}

write_at_bold() {
    local x=$1 y=$2
    shift 2
    local text="$1"
    local fg="${2:-$C_WHITE}"
    tput cup "$y" "$x"
    tput bold
    tput setaf "$fg"
    printf '%s' "$text"
    tput sgr0
}

clear_row() {
    local y=$1
    local start_x="${2:-0}"
    tput cup "$y" "$start_x"
    printf '%*s' $((TERM_COLS - start_x)) ''
}

clear_rows() {
    local start=$1 count=$2
    for ((i = 0; i < count; i++)); do
        clear_row $((start + i))
    done
}

truncate_str() {
    local str="$1" max="$2"
    if (( ${#str} <= max )); then
        printf '%s' "$str"
    else
        printf '%s~' "${str:0:$((max - 1))}"
    fi
}

pad_right() {
    local str="$1" width="$2"
    printf "%-${width}s" "$str"
}

format_size() {
    local bytes=$1
    if (( bytes <= 0 )); then
        printf '? MB'
        return
    fi
    if (( bytes >= 1073741824 )); then
        local gb=$(( (bytes * 10) / 1073741824 ))
        printf '%d.%d GB' $((gb / 10)) $((gb % 10))
    else
        local mb=$(( (bytes * 10) / 1048576 ))
        printf '%d.%d MB' $((mb / 10)) $((mb % 10))
    fi
}

format_size_mb() {
    local bytes=$1
    local mb=$(( (bytes * 10) / 1048576 ))
    printf '%d.%d' $((mb / 10)) $((mb % 10))
}

# Read a single keypress, handling escape sequences for arrows
read_key() {
    local key
    IFS= read -rsn1 key
    if [[ "$key" == $'\x1b' ]]; then
        local seq
        IFS= read -rsn1 -t 0.1 seq
        if [[ "$seq" == "[" ]]; then
            IFS= read -rsn1 -t 0.1 seq
            case "$seq" in
                A) echo "UP" ;;
                B) echo "DOWN" ;;
                C) echo "RIGHT" ;;
                D) echo "LEFT" ;;
                *) echo "ESC" ;;
            esac
        else
            echo "ESC"
        fi
    elif [[ "$key" == "" ]]; then
        echo "ENTER"
    elif [[ "$key" == $'\x7f' ]] || [[ "$key" == $'\x08' ]]; then
        echo "BACKSPACE"
    else
        echo "$key"
    fi
}

draw_banner() {
    local y="${1:-0}"
    local line1="+========================================+"
    local line2="|        UnoDOS Disk Writer              |"
    local line3="+========================================+"
    local w=${#line1}
    local pad=$(( (TERM_COLS - w) / 2 ))
    (( pad < 0 )) && pad=0

    write_at_bold $pad $((y))     "$line1" $C_YELLOW
    write_at_bold $pad $((y + 1)) "$line2" $C_YELLOW
    write_at_bold $pad $((y + 2)) "$line3" $C_YELLOW
    return 0
}

# ─── Image Detection ─────────────────────────────────────────────────────────

get_image_description() {
    local filename="$1"
    case "$filename" in
        unodos-144*)      echo "OS + Apps (floppy)" ;;
        launcher-floppy*) echo "Apps-only floppy" ;;
        unodos-hd*)       echo "HD / CF card" ;;
        apps-floppy*)     echo "Apps-only floppy" ;;
        *)                echo "" ;;
    esac
}

find_images() {
    IMG_COUNT=0
    local -a preferred=("unodos-144.img" "launcher-floppy.img" "unodos-hd.img")
    local -a found_paths=()

    # Preferred order first
    for name in "${preferred[@]}"; do
        local path="$BUILD_DIR/$name"
        if [[ -f "$path" ]]; then
            found_paths+=("$path")
        fi
    done

    # Then any other .img files
    if [[ -d "$BUILD_DIR" ]]; then
        for f in "$BUILD_DIR"/*.img; do
            [[ -f "$f" ]] || continue
            local base
            base=$(basename "$f")
            local already=0
            for p in "${preferred[@]}"; do
                [[ "$base" == "$p" ]] && already=1 && break
            done
            (( already )) || found_paths+=("$f")
        done
    fi

    for path in "${found_paths[@]}"; do
        local name size version build desc
        name=$(basename "$path")
        size=$(stat -c%s "$path" 2>/dev/null || echo 0)
        version=$(strings "$path" 2>/dev/null | grep -oP 'UnoDOS v[\d.]+' | head -1 || echo "")
        build=$(strings "$path" 2>/dev/null | grep -oP 'Build: \d+' | head -1 || echo "")
        desc=$(get_image_description "$name")

        IMG_PATHS[$IMG_COUNT]="$path"
        IMG_NAMES[$IMG_COUNT]="$name"
        IMG_SIZES[$IMG_COUNT]="$size"
        IMG_VERSIONS[$IMG_COUNT]="$version"
        IMG_BUILDS[$IMG_COUNT]="$build"
        IMG_DESCS[$IMG_COUNT]="$desc"
        (( IMG_COUNT++ ))
    done
}

# ─── Drive Detection ─────────────────────────────────────────────────────────

is_system_device() {
    local dev="/dev/$1"
    # Check if any partition of this device is mounted on critical paths
    local mounts
    mounts=$(lsblk -nro MOUNTPOINT "$dev" 2>/dev/null | grep -v '^$' || true)
    while IFS= read -r mp; do
        case "$mp" in
            /|/boot|/boot/*|/home|/home/*|"[SWAP]")
                return 0 ;;
        esac
    done <<< "$mounts"
    return 1
}

find_drives() {
    DRV_COUNT=0

    # Check for floppy drives
    for fd in /dev/fd0 /dev/fd1; do
        if [[ -b "$fd" ]]; then
            local name
            name=$(basename "$fd")
            DRV_NAMES[$DRV_COUNT]="$name"
            DRV_PATHS[$DRV_COUNT]="$fd"
            DRV_SIZES[$DRV_COUNT]="1.4 MB"
            DRV_SIZE_BYTES[$DRV_COUNT]=1474560
            DRV_MODELS[$DRV_COUNT]="Floppy Drive"
            DRV_TRANS[$DRV_COUNT]="floppy"
            DRV_REMOVABLE[$DRV_COUNT]=1
            DRV_TYPES[$DRV_COUNT]="Floppy"
            (( DRV_COUNT++ ))
        fi
    done

    # Enumerate block devices using lsblk --pairs format (robust parsing)
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue

        # Parse KEY="VALUE" pairs safely using regex
        local name="" size="" type="" tran="" model="" rm="" size_bytes=0
        [[ "$line" =~ NAME=\"([^\"]*)\".* ]] && name="${BASH_REMATCH[1]}"
        [[ "$line" =~ SIZE=\"([^\"]*)\".* ]] && size="${BASH_REMATCH[1]}"
        [[ "$line" =~ TYPE=\"([^\"]*)\".* ]] && type="${BASH_REMATCH[1]}"
        [[ "$line" =~ TRAN=\"([^\"]*)\".* ]] && tran="${BASH_REMATCH[1]}"
        [[ "$line" =~ MODEL=\"([^\"]*)\".* ]] && model="${BASH_REMATCH[1]}"
        [[ "$line" =~ RM=\"([^\"]*)\".* ]] && rm="${BASH_REMATCH[1]}"

        # Skip non-disk devices
        [[ "$type" != "disk" ]] && continue
        [[ "$name" == loop* || "$name" == ram* || "$name" == sr* || "$name" == fd* ]] && continue

        # Skip system devices
        if is_system_device "$name"; then
            continue
        fi

        # Get size in bytes from sysfs
        if [[ -f "/sys/block/$name/size" ]]; then
            size_bytes=$(( $(cat "/sys/block/$name/size") * 512 ))
        fi

        [[ -z "$model" ]] && model="Unknown"
        [[ -z "$tran" ]] && tran="?"
        [[ -z "$rm" ]] && rm="0"

        DRV_NAMES[$DRV_COUNT]="$name"
        DRV_PATHS[$DRV_COUNT]="/dev/$name"
        DRV_SIZES[$DRV_COUNT]="$size"
        DRV_SIZE_BYTES[$DRV_COUNT]="$size_bytes"
        DRV_MODELS[$DRV_COUNT]="$model"
        DRV_TRANS[$DRV_COUNT]="$tran"
        DRV_REMOVABLE[$DRV_COUNT]="$rm"
        DRV_TYPES[$DRV_COUNT]="Disk"
        (( DRV_COUNT++ ))
    done < <(lsblk -dPo NAME,SIZE,TYPE,TRAN,MODEL,RM 2>/dev/null)
}

# ─── Render Functions ─────────────────────────────────────────────────────────

render_image_list() {
    local sel=$1 top=$2
    local line_w=$((TERM_COLS - 6))
    (( line_w > 78 )) && line_w=78

    for ((i = 0; i < IMG_COUNT; i++)); do
        local y=$((top + i))
        local name ver_str desc
        name=$(pad_right "$(truncate_str "${IMG_NAMES[$i]}" 21)" 22)
        local size_str
        size_str=$(pad_right "$(format_size "${IMG_SIZES[$i]}")" 10)
        ver_str="${IMG_VERSIONS[$i]} ${IMG_BUILDS[$i]}"
        ver_str="${ver_str## }"
        ver_str="${ver_str%% }"
        desc="${IMG_DESCS[$i]}"

        clear_row "$y" 3

        if (( i == sel )); then
            # Selected: white on dark blue background
            tput cup "$y" 3
            tput setab $C_WHITE; tput setaf $C_BLACK
            printf '%-*s' "$line_w" ""
            tput cup "$y" 4
            tput setab $C_WHITE; tput setaf $C_BLACK
            printf '[>] '
            printf '%s' "$name"
            printf '%s' "$size_str"
            tput setab $C_WHITE; tput setaf $C_BLACK
            printf '%s' "$ver_str"
            if [[ -n "$desc" ]]; then
                printf '  %s' "$desc"
            fi
            tput sgr0
        else
            write_at 4 "$y" "[ ]" $C_GRAY
            write_at 8 "$y" "$name" $C_WHITE
            write_at 30 "$y" "$size_str" $C_GRAY
            write_at 40 "$y" "$ver_str" $C_CYAN
            if [[ -n "$desc" ]]; then
                write_at 60 "$y" "$(truncate_str "$desc" $((line_w - 57)))" $C_GRAY
            fi
        fi
    done
}

render_drive_list() {
    local sel=$1 top=$2 image_size=$3
    local line_w=$((TERM_COLS - 6))
    (( line_w > 78 )) && line_w=78

    for ((i = 0; i < DRV_COUNT; i++)); do
        local y=$((top + i))
        local dev_type dev_name dev_model dev_size dev_trans suffix=""
        dev_type=$(pad_right "${DRV_TYPES[$i]}" 8)
        dev_name=$(pad_right "${DRV_NAMES[$i]}" 8)
        dev_model=$(pad_right "$(truncate_str "${DRV_MODELS[$i]}" 26)" 26)
        dev_size=$(pad_right "${DRV_SIZES[$i]}" 10)
        dev_trans=$(pad_right "${DRV_TRANS[$i]}" 8)

        local size_b="${DRV_SIZE_BYTES[$i]}"
        local too_small=0 is_large=0
        if [[ "${DRV_TYPES[$i]}" != "Floppy" ]] && (( size_b > 0 && size_b < image_size )); then
            too_small=1
            suffix="(too small)"
        elif (( size_b > 274877906944 )); then  # 256 GB
            is_large=1
            suffix="[!] >256GB"
        fi

        clear_row "$y" 3

        if (( i == sel )); then
            tput cup "$y" 3
            tput setab $C_WHITE; tput setaf $C_BLACK
            printf '%-*s' "$line_w" ""
            tput cup "$y" 4
            tput setab $C_WHITE; tput setaf $C_BLACK
            printf '%s' "$dev_type"
            printf '%s' "$dev_name"
            printf '%s' "$dev_model"
            printf '%s' "$dev_size"
            printf '%s' "$dev_trans"
            if [[ -n "$suffix" ]]; then
                tput setaf $C_YELLOW
                printf '%s' "$suffix"
            fi
            tput sgr0
        elif (( too_small )); then
            write_at 4 "$y" "$dev_type" $C_GRAY
            write_at 12 "$y" "$dev_name" $C_GRAY
            write_at 20 "$y" "$dev_model" $C_GRAY
            write_at 46 "$y" "$dev_size" $C_GRAY
            write_at 56 "$y" "$dev_trans" $C_GRAY
            write_at 64 "$y" "$suffix" $C_RED
        elif (( is_large )); then
            write_at 4 "$y" "$dev_type" $C_GRAY
            write_at 12 "$y" "$dev_name" $C_GRAY
            write_at 20 "$y" "$dev_model" $C_GRAY
            write_at 46 "$y" "$dev_size" $C_GRAY
            write_at 56 "$y" "$dev_trans" $C_GRAY
            write_at 64 "$y" "$suffix" $C_YELLOW
        else
            write_at 4 "$y" "$dev_type" $C_WHITE
            write_at 12 "$y" "$dev_name" $C_WHITE
            write_at 20 "$y" "$dev_model" $C_WHITE
            write_at 46 "$y" "$dev_size" $C_GRAY
            write_at 56 "$y" "$dev_trans" $C_GRAY
        fi
    done

    # Info area below list
    local info_row=$((top + DRV_COUNT + 1))
    clear_row "$info_row"
    clear_row $((info_row + 1))

    if (( DRV_COUNT > 0 )); then
        local sb="${DRV_SIZE_BYTES[$sel]}"
        if [[ "${DRV_TYPES[$sel]}" != "Floppy" ]] && (( sb > 0 && sb < image_size )); then
            write_at 3 "$info_row" "ERROR: Drive is too small for this image!" $C_RED
        elif (( sb > 274877906944 )); then
            write_at 3 "$info_row" "WARNING: Drive is larger than 256 GB - verify this is correct!" $C_YELLOW
        elif (( sb > 34359738368 )); then  # 32 GB
            write_at 3 "$info_row" "NOTE: Drive is larger than 32 GB" $C_YELLOW
        fi
    fi
}

render_buttons() {
    local sel=$1 y=$2
    if (( sel == 0 )); then
        # YES selected
        tput cup "$y" 10
        tput setab $C_GREEN; tput setaf $C_BLACK
        printf '[ YES ]'
        tput sgr0
        write_at 22 "$y" "  No   " $C_GRAY
    else
        # NO selected
        write_at 10 "$y" "  Yes  " $C_GRAY
        tput cup "$y" 22
        tput setab $C_RED; tput setaf $C_BLACK
        printf '[  NO  ]'
        tput sgr0
    fi
}

draw_progress_bar() {
    local y=$1 x=$2 width=$3 percent=$4
    local filled=$(( (percent * width) / 100 ))
    local empty=$(( width - filled ))

    tput cup "$y" "$x"
    tput setaf $C_CYAN
    printf '['
    printf '%*s' "$filled" '' | tr ' ' '#'
    tput setaf $C_GRAY
    printf '%*s' "$empty" '' | tr ' ' '-'
    tput setaf $C_CYAN
    printf ']'
    tput sgr0
    printf ' %3d%%' "$percent"
}

# ─── Screen Functions ─────────────────────────────────────────────────────────

screen_image_select() {
    tput clear
    local row=0
    draw_banner 0
    row=3
    ((row++))

    write_at 3 "$row" "Select image to write:" $C_WHITE
    write_at 55 "$row" "Step 1 of 3" $C_GRAY
    ((row++))
    write_at 3 "$row" "Up/Down = navigate   Enter = next   Esc = cancel" $C_GRAY
    ((row += 2))

    find_images

    if (( IMG_COUNT == 0 )); then
        write_at 3 "$row" "No images found in build/ directory!" $C_RED
        ((row++))
        write_at 3 "$row" "Run 'make floppy144' or 'git pull' to get images." $C_YELLOW
        ((row += 2))
        write_at 3 "$row" "Press any key to exit..." $C_GRAY
        read_key >/dev/null
        return 1
    fi

    local list_top=$row
    local img_sel=0
    render_image_list $img_sel $list_top

    while true; do
        local key
        key=$(read_key)
        case "$key" in
            UP)
                if (( img_sel > 0 )); then
                    (( img_sel-- ))
                    render_image_list $img_sel $list_top
                fi
                ;;
            DOWN)
                if (( img_sel < IMG_COUNT - 1 )); then
                    (( img_sel++ ))
                    render_image_list $img_sel $list_top
                fi
                ;;
            ENTER)
                SELECTED_IMAGE="${IMG_PATHS[$img_sel]}"
                SELECTED_IMAGE_IDX=$img_sel
                return 0
                ;;
            ESC|q)
                return 1
                ;;
        esac
    done
}

screen_drive_select() {
    local can_go_back=$1
    tput clear
    local row=0
    draw_banner 0
    row=3
    ((row++))

    local image_name image_size_mb
    image_name=$(basename "$SELECTED_IMAGE")
    image_size_mb=$(format_size_mb "$(stat -c%s "$SELECTED_IMAGE")")

    write_at 3 "$row" "Image:  $image_name ($image_size_mb MB)" $C_WHITE
    ((row++))

    if (( SELECTED_IMAGE_IDX >= 0 )); then
        local ver="${IMG_VERSIONS[$SELECTED_IMAGE_IDX]} ${IMG_BUILDS[$SELECTED_IMAGE_IDX]}"
        ver="${ver## }"
        ver="${ver%% }"
        if [[ -n "$ver" ]]; then
            write_at 11 "$row" "$ver" $C_CYAN
            ((row++))
        fi
    fi
    ((row++))

    local step_num=1 step_total=2
    if (( can_go_back )); then
        step_num=2
        step_total=3
    fi

    write_at 3 "$row" "Select target drive:" $C_WHITE
    write_at 55 "$row" "Step $step_num of $step_total" $C_GRAY
    ((row++))

    local hints="Up/Down = navigate   Enter = next"
    (( can_go_back )) && hints+="   Bksp = back"
    hints+="   Esc = cancel"
    write_at 3 "$row" "$hints" $C_GRAY
    ((row += 2))

    # Table header
    write_at 4 "$row" "Type" $C_CYAN
    write_at 12 "$row" "Device" $C_CYAN
    write_at 20 "$row" "Model" $C_CYAN
    write_at 46 "$row" "Size" $C_CYAN
    write_at 56 "$row" "Bus" $C_CYAN
    ((row++))
    local line_w=$((TERM_COLS - 6))
    (( line_w > 74 )) && line_w=74
    tput cup "$row" 3
    tput setaf $C_GRAY
    printf '%*s' "$line_w" '' | tr ' ' '-'
    tput sgr0
    ((row++))

    write_at 3 "$row" "Scanning drives..." $C_CYAN
    find_drives
    clear_row "$row"

    if (( DRV_COUNT == 0 )); then
        write_at 3 "$row" "No eligible drives found!" $C_RED
        ((row++))
        write_at 3 "$row" "(System and boot drives are automatically excluded)" $C_GRAY
        ((row += 2))
        write_at 3 "$row" "Connect a floppy, CF card, or USB drive and try again." $C_YELLOW
        ((row += 2))
        write_at 3 "$row" "Press any key to exit..." $C_GRAY
        read_key >/dev/null
        return 1
    fi

    local list_top=$row
    local drv_sel=0
    local image_size
    image_size=$(stat -c%s "$SELECTED_IMAGE")
    render_drive_list $drv_sel $list_top "$image_size"

    while true; do
        local key
        key=$(read_key)
        case "$key" in
            UP)
                if (( drv_sel > 0 )); then
                    (( drv_sel-- ))
                    render_drive_list $drv_sel $list_top "$image_size"
                fi
                ;;
            DOWN)
                if (( drv_sel < DRV_COUNT - 1 )); then
                    (( drv_sel++ ))
                    render_drive_list $drv_sel $list_top "$image_size"
                fi
                ;;
            ENTER)
                # Block if drive too small (skip for floppies)
                local sb="${DRV_SIZE_BYTES[$drv_sel]}"
                if [[ "${DRV_TYPES[$drv_sel]}" != "Floppy" ]] && \
                   (( sb > 0 && sb < image_size )); then
                    # Flash error, don't proceed
                    :
                else
                    SELECTED_DEVICE="${DRV_PATHS[$drv_sel]}"
                    SELECTED_DEVICE_IDX=$drv_sel
                    return 0
                fi
                ;;
            BACKSPACE)
                if (( can_go_back )); then
                    return 2  # Go back
                fi
                ;;
            ESC|q)
                return 1
                ;;
        esac
    done
}

screen_confirm() {
    local can_go_back=$1
    tput clear
    local row=0
    draw_banner 0
    row=3
    ((row++))

    local image_name image_size_mb
    image_name=$(basename "$SELECTED_IMAGE")
    image_size_mb=$(format_size_mb "$(stat -c%s "$SELECTED_IMAGE")")

    local step_total=1
    [[ "$first_screen" == "image" ]] && step_total=3
    [[ "$first_screen" == "drive" ]] && step_total=2

    write_at_bold 3 "$row" "CONFIRM WRITE" $C_RED
    write_at 55 "$row" "Step $step_total of $step_total" $C_GRAY
    ((row += 2))

    write_at 5 "$row" "Image:" $C_GRAY
    write_at 14 "$row" "$image_name ($image_size_mb MB)" $C_WHITE
    ((row++))

    if (( SELECTED_IMAGE_IDX >= 0 )); then
        local ver="${IMG_VERSIONS[$SELECTED_IMAGE_IDX]} ${IMG_BUILDS[$SELECTED_IMAGE_IDX]}"
        ver="${ver## }"
        ver="${ver%% }"
        if [[ -n "$ver" ]]; then
            write_at 14 "$row" "$ver" $C_CYAN
            ((row++))
        fi
    fi
    ((row++))

    local target_label
    if [[ "${DRV_TYPES[$SELECTED_DEVICE_IDX]}" == "Floppy" ]]; then
        target_label="${DRV_NAMES[$SELECTED_DEVICE_IDX]} - ${DRV_MODELS[$SELECTED_DEVICE_IDX]}"
    else
        target_label="${DRV_PATHS[$SELECTED_DEVICE_IDX]} - ${DRV_MODELS[$SELECTED_DEVICE_IDX]}"
    fi
    local target_size="${DRV_SIZES[$SELECTED_DEVICE_IDX]}"
    local target_trans="${DRV_TRANS[$SELECTED_DEVICE_IDX]}"

    write_at 5 "$row" "Target:" $C_GRAY
    write_at 14 "$row" "$(truncate_str "$target_label" 50)" $C_WHITE
    ((row++))
    write_at 14 "$row" "$target_size   $target_trans" $C_GRAY
    ((row += 2))

    # Size warnings
    local sb="${DRV_SIZE_BYTES[$SELECTED_DEVICE_IDX]}"
    if (( sb > 274877906944 )); then
        write_at_bold 5 "$row" "WARNING: This drive is very large (>256 GB)!" $C_RED
        ((row++))
        write_at_bold 5 "$row" "Double-check this is the correct target!" $C_RED
        ((row += 2))
    elif (( sb > 34359738368 )); then
        write_at 5 "$row" "NOTE: Drive is larger than 32 GB - verify this is correct." $C_YELLOW
        ((row += 2))
    fi

    # Warning banner
    write_at_bold 3 "$row" "+----------------------------------------------+" $C_RED
    ((row++))
    write_at_bold 3 "$row" "|  ALL DATA ON THIS DISK WILL BE ERASED!       |" $C_RED
    ((row++))
    write_at_bold 3 "$row" "|  THIS CANNOT BE UNDONE!                      |" $C_RED
    ((row++))
    write_at_bold 3 "$row" "+----------------------------------------------+" $C_RED
    ((row += 2))

    write_at 5 "$row" "Erase this disk and write UnoDOS?" $C_WHITE
    ((row += 2))

    local btn_row=$row
    local btn_sel=1  # Default to No
    render_buttons $btn_sel $btn_row

    ((row += 2))
    local hints="Left/Right or Y/N   Enter = confirm"
    (( can_go_back )) && hints+="   Bksp = back"
    hints+="   Esc = cancel"
    write_at 3 "$row" "$hints" $C_GRAY

    while true; do
        local key
        key=$(read_key)
        case "$key" in
            LEFT|RIGHT)
                btn_sel=$(( 1 - btn_sel ))
                render_buttons $btn_sel $btn_row
                ;;
            y|Y)
                btn_sel=0
                render_buttons $btn_sel $btn_row
                # Extra confirm for >256GB
                if (( sb > 274877906944 )); then
                    if ! extra_confirm $((btn_row + 4)); then
                        return 1
                    fi
                fi
                return 0  # Confirmed
                ;;
            n|N|ESC)
                return 1
                ;;
            BACKSPACE)
                if (( can_go_back )); then
                    return 2  # Go back to drive selection
                fi
                ;;
            ENTER)
                if (( btn_sel == 0 )); then
                    # Extra confirm for >256GB
                    if (( sb > 274877906944 )); then
                        if ! extra_confirm $((btn_row + 4)); then
                            return 1
                        fi
                    fi
                    return 0  # Confirmed
                else
                    return 1  # Cancelled
                fi
                ;;
        esac
    done
}

extra_confirm() {
    local row=$1
    write_at_bold 5 "$row" "This drive is >256 GB. Are you REALLY sure?" $C_RED
    ((row += 2))
    local btn2_row=$row
    local btn2_sel=1  # Default No
    render_buttons $btn2_sel $btn2_row

    while true; do
        local key
        key=$(read_key)
        case "$key" in
            LEFT|RIGHT)
                btn2_sel=$(( 1 - btn2_sel ))
                render_buttons $btn2_sel $btn2_row
                ;;
            y|Y)
                return 0
                ;;
            n|N|ESC)
                return 1
                ;;
            ENTER)
                if (( btn2_sel == 0 )); then
                    return 0
                else
                    return 1
                fi
                ;;
        esac
    done
}

screen_write() {
    tput clear
    local row=0
    draw_banner 0
    row=3
    ((row++))

    local image_name target_label
    image_name=$(basename "$SELECTED_IMAGE")
    if [[ "${DRV_TYPES[$SELECTED_DEVICE_IDX]}" == "Floppy" ]]; then
        target_label="${DRV_NAMES[$SELECTED_DEVICE_IDX]} - ${DRV_MODELS[$SELECTED_DEVICE_IDX]}"
    else
        target_label="${DRV_PATHS[$SELECTED_DEVICE_IDX]} - ${DRV_MODELS[$SELECTED_DEVICE_IDX]}"
    fi

    write_at 3 "$row" "Writing $image_name to $target_label..." $C_WHITE
    ((row += 2))

    local image_size
    image_size=$(stat -c%s "$SELECTED_IMAGE")
    local image_size_mb
    image_size_mb=$(format_size_mb "$image_size")

    # Determine block size
    local bs="1M"
    if [[ "${DRV_TYPES[$SELECTED_DEVICE_IDX]}" == "Floppy" ]]; then
        bs="32K"
    fi

    # Unmount any mounted partitions on the target
    write_at 5 "$row" "[1/3] Preparing drive..." $C_CYAN
    for part in "${SELECTED_DEVICE}"*; do
        umount "$part" 2>/dev/null || true
    done
    write_at 5 "$row" "[1/3] Preparing drive... Done     " $C_GREEN
    ((row++))

    write_at 5 "$row" "[2/3] Writing image..." $C_CYAN
    ((row++))

    local bar_x=5
    local bar_w=50
    (( bar_w > TERM_COLS - 20 )) && bar_w=$((TERM_COLS - 20))
    local bar_row=$row
    ((row++))
    local status_row=$row
    ((row++))

    draw_progress_bar "$bar_row" "$bar_x" "$bar_w" 0

    # Write using dd with progress output on stderr
    # dd status=progress writes lines like: "1048576 bytes (1.0 MB, 1.0 MiB) copied, 0.5 s, 2.1 MB/s"
    local dd_err_file="/tmp/dd_err_$$"
    dd if="$SELECTED_IMAGE" of="$SELECTED_DEVICE" bs="$bs" conv=fsync 2>"$dd_err_file" &
    local dd_pid=$!

    local start_time
    start_time=$(date +%s)

    # Monitor progress by periodically sending USR1 to dd and parsing stderr
    while kill -0 $dd_pid 2>/dev/null; do
        sleep 1
        kill -USR1 $dd_pid 2>/dev/null || true
        sleep 0.3

        # Parse the last progress line from dd stderr
        local last_line bytes_written=0
        last_line=$(tail -1 "$dd_err_file" 2>/dev/null || echo "")
        if [[ "$last_line" =~ ^([0-9]+)\ bytes ]]; then
            bytes_written="${BASH_REMATCH[1]}"
        fi

        if (( bytes_written > 0 && image_size > 0 )); then
            local percent=$(( (bytes_written * 100) / image_size ))
            (( percent > 100 )) && percent=100
            draw_progress_bar "$bar_row" "$bar_x" "$bar_w" "$percent"

            local now elapsed speed mb_done
            now=$(date +%s)
            elapsed=$(( now - start_time ))
            if (( elapsed > 0 )); then
                speed=$(( bytes_written / 1048576 / elapsed ))
                mb_done=$(format_size_mb "$bytes_written")
                write_at 5 "$status_row" "$mb_done / $image_size_mb MB   (${speed} MB/s)     " $C_GRAY
            fi
        fi
    done

    # Wait for dd to finish and check exit status
    wait $dd_pid
    local dd_exit=$?
    rm -f "$dd_err_file"

    if (( dd_exit != 0 )); then
        ((row++))
        write_at_bold 5 "$row" "ERROR: Write failed!" $C_RED
        ((row += 2))
        write_at 5 "$row" "Press any key to exit..." $C_GRAY
        read_key >/dev/null
        return 1
    fi

    # Final progress
    draw_progress_bar "$bar_row" "$bar_x" "$bar_w" 100
    write_at 5 "$status_row" "$image_size_mb / $image_size_mb MB   Done!              " $C_GRAY

    ((row++))
    write_at 5 "$row" "[2/3] Write complete!" $C_GREEN
    ((row++))

    write_at 5 "$row" "[3/3] Syncing..." $C_CYAN
    sync
    write_at 5 "$row" "[3/3] Syncing... Done     " $C_GREEN
    ((row += 2))

    # Verification
    if (( ARG_VERIFY )); then
        write_at 5 "$row" "Verifying..." $C_CYAN
        if cmp -n "$image_size" "$SELECTED_DEVICE" "$SELECTED_IMAGE" 2>/dev/null; then
            write_at 5 "$row" "Verifying... PASSED!                   " $C_GREEN
        else
            write_at 5 "$row" "Verifying... FAILED! Data mismatch" $C_RED
        fi
        ((row += 2))
    fi

    # Success banner
    write_at_bold 3 "$row" "+========================================+" $C_GREEN
    ((row++))
    local success_msg="     Write Complete! ($image_size_mb MB)"
    local pad_len=$(( 41 - ${#success_msg} - 1 ))
    (( pad_len < 0 )) && pad_len=0
    write_at_bold 3 "$row" "|${success_msg}$(printf '%*s' $pad_len '')|" $C_GREEN
    ((row++))
    write_at_bold 3 "$row" "+========================================+" $C_GREEN
    ((row += 2))

    # Context-sensitive next steps
    local desc
    desc=$(get_image_description "$image_name")
    if [[ "${DRV_TYPES[$SELECTED_DEVICE_IDX]}" == "Floppy" ]]; then
        write_at 5 "$row" "Next steps:" $C_WHITE
        ((row++))
        write_at 5 "$row" "1. Boot from this floppy - launcher will auto-load!" $C_WHITE
        ((row++))
        if [[ "$desc" == *"Apps"* ]]; then
            write_at 5 "$row" "2. Or swap to this disk while launcher is running" $C_WHITE
            ((row++))
        fi
    else
        write_at 5 "$row" "Next steps:" $C_WHITE
        ((row++))
        write_at 5 "$row" "1. Remove the CF card / USB drive" $C_WHITE
        ((row++))
        write_at 5 "$row" "2. Install in target machine" $C_WHITE
        ((row++))
        write_at 5 "$row" "3. Boot from hard drive to run UnoDOS" $C_WHITE
        ((row++))
    fi

    ((row += 2))
    write_at 5 "$row" "Press any key to exit..." $C_GRAY
    read_key >/dev/null
    return 0
}

# ─── Usage ────────────────────────────────────────────────────────────────────

usage() {
    cat << 'EOF'
UnoDOS Disk Image Writer - Linux TUI

Usage: sudo ./tools/write.sh [options]

Options:
  -i, --image PATH    Image file path (skip image selection)
  -d, --device PATH   Device path e.g. /dev/sdc (skip drive selection)
  -v, --verify        Verify after writing
  -h, --help          Show this help

No arguments = full interactive mode (image selection + drive selection)

Examples:
  sudo ./tools/write.sh                              # Full interactive
  sudo ./tools/write.sh -i build/unodos-144.img      # Select drive only
  sudo ./tools/write.sh -i build/unodos-hd.img -d /dev/sdc   # Direct write
  sudo ./tools/write.sh -v                            # Interactive + verify
EOF
}

# ─── Main ─────────────────────────────────────────────────────────────────────

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -i|--image)
            ARG_IMAGE="$2"
            shift 2
            ;;
        -d|--device)
            ARG_DEVICE="$2"
            shift 2
            ;;
        -v|--verify)
            ARG_VERIFY=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Check root
if (( EUID != 0 )); then
    echo "ERROR: This script must be run as root (sudo)."
    echo "Usage: sudo $0 [options]"
    exit 1
fi

# Check dependencies
for cmd in tput lsblk dd strings; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "ERROR: Required command '$cmd' not found."
        exit 1
    fi
done

# Resolve CLI arguments
if [[ -n "$ARG_IMAGE" ]]; then
    if [[ ! -f "$ARG_IMAGE" ]]; then
        echo "ERROR: Image not found: $ARG_IMAGE"
        exit 1
    fi
    SELECTED_IMAGE="$(realpath "$ARG_IMAGE")"
    # Get image info for display
    find_images
    for ((i = 0; i < IMG_COUNT; i++)); do
        if [[ "${IMG_PATHS[$i]}" == "$SELECTED_IMAGE" ]]; then
            SELECTED_IMAGE_IDX=$i
            break
        fi
    done
fi

if [[ -n "$ARG_DEVICE" ]]; then
    if [[ ! -b "$ARG_DEVICE" ]]; then
        echo "ERROR: Device not found or not a block device: $ARG_DEVICE"
        exit 1
    fi
    SELECTED_DEVICE="$ARG_DEVICE"
    # Find this device in drive list
    find_drives
    for ((i = 0; i < DRV_COUNT; i++)); do
        if [[ "${DRV_PATHS[$i]}" == "$SELECTED_DEVICE" ]]; then
            SELECTED_DEVICE_IDX=$i
            break
        fi
    done
    # If not in filtered list, add it manually
    if (( SELECTED_DEVICE_IDX < 0 )); then
        _dev_name=$(basename "$ARG_DEVICE")
        _dev_size_bytes=0
        if [[ -f "/sys/block/$_dev_name/size" ]]; then
            _dev_size_bytes=$(( $(cat "/sys/block/$_dev_name/size") * 512 ))
        fi
        _dev_model=$(lsblk -dno MODEL "$ARG_DEVICE" 2>/dev/null || echo "Unknown")
        DRV_NAMES[$DRV_COUNT]="$_dev_name"
        DRV_PATHS[$DRV_COUNT]="$ARG_DEVICE"
        DRV_SIZES[$DRV_COUNT]=$(format_size "$_dev_size_bytes")
        DRV_SIZE_BYTES[$DRV_COUNT]="$_dev_size_bytes"
        DRV_MODELS[$DRV_COUNT]="$_dev_model"
        DRV_TRANS[$DRV_COUNT]="?"
        DRV_REMOVABLE[$DRV_COUNT]=0
        DRV_TYPES[$DRV_COUNT]="Disk"
        SELECTED_DEVICE_IDX=$DRV_COUNT
        (( DRV_COUNT++ ))
    fi
fi

# Determine starting screen
first_screen="image"
if [[ -n "$SELECTED_IMAGE" ]]; then
    first_screen="drive"
fi
if [[ -n "$SELECTED_IMAGE" && -n "$SELECTED_DEVICE" ]]; then
    first_screen="confirm"
fi

# Initialize TUI
init_tui

nav_screen="$first_screen"
ret=0
can_go_back=0

while true; do
    case "$nav_screen" in
        image)
            screen_image_select
            ret=$?
            if (( ret == 0 )); then
                nav_screen="drive"
            else
                cleanup
                echo "Operation cancelled."
                exit 0
            fi
            ;;
        drive)
            can_go_back=0
            [[ "$first_screen" == "image" ]] && can_go_back=1
            screen_drive_select $can_go_back
            ret=$?
            if (( ret == 0 )); then
                nav_screen="confirm"
            elif (( ret == 2 )); then
                nav_screen="image"
            else
                cleanup
                echo "Operation cancelled."
                exit 0
            fi
            ;;
        confirm)
            can_go_back=0
            [[ -z "$ARG_DEVICE" ]] && can_go_back=1
            screen_confirm $can_go_back
            ret=$?
            if (( ret == 0 )); then
                nav_screen="write"
            elif (( ret == 2 )); then
                nav_screen="drive"
            else
                cleanup
                echo "Operation cancelled."
                exit 0
            fi
            ;;
        write)
            screen_write
            break
            ;;
    esac
done

cleanup
exit 0
