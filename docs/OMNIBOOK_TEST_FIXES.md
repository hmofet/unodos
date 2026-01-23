# Omnibook Test Fixes (v3.10.0 Patch 1)

## Issues Found During Initial Testing

### Issue 1: Doesn't Wait After Pressing F
**Symptom:** After pressing F to start the filesystem test, the system immediately tries to read the disk without waiting for a keypress.

**Root Cause:** The 'F' keypress event is stored in both:
- The event queue (consumed by `event_wait_stub`)
- The keyboard buffer (not consumed)

When `test_filesystem()` calls `kbd_wait_key()`, it immediately returns with the 'F' still in the buffer.

**Fix:** Added `clear_kbd_buffer()` function that drains both:
- Keyboard buffer (via `kbd_getchar()` loop)
- Event queue (via `event_get_stub()` loop)

This function is called before waiting for the user's keypress to insert the test floppy.

**Status:** ✅ FIXED

---

### Issue 2: Mount: FAIL
**Symptom:** FAT12 mount fails on the HP Omnibook 600C with real floppy hardware.

**Potential Root Causes:**
1. Disk system not reset after floppy swap
2. Drive not spun up / not ready
3. Transient read errors
4. Invalid or non-FAT12 formatted floppy
5. BIOS timing issues

**Fixes Applied:**

#### 1. Disk System Reset (INT 13h AH=00)
Before reading the boot sector, the disk system is now reset:
```asm
xor ax, ax          ; AH=00 (reset disk system)
xor dx, dx          ; DL=0 (drive A:)
int 0x13
```
This ensures the drive is in a known state, especially important after swapping floppies.

#### 2. Spin-Up Delays
Added two delay loops:
- **After disk reset:** 0x4000 iterations (for initial spin-up)
- **After floppy swap:** 0x8000 iterations (in test_filesystem)

Mechanical floppy drives need time to:
- Detect the new disk
- Spin up to operating speed
- Position the read head

#### 3. Read Retry Logic
Boot sector reads now retry up to 3 times on failure:
```asm
mov bp, 3           ; Retry count
.retry_read:
    ; ... INT 13h read ...
    jnc .read_success
    dec bp
    jnz .retry_read
```

This handles:
- Transient read errors
- Drive not ready on first attempt
- Timing-sensitive hardware

#### 4. Boot Sector Validation
After successful read, the BPB is validated:
- **Signature check:** Offset 510-511 must be 0xAA55
- **Bytes per sector:** Must be 512 (FAT12 standard)
- **Sectors per cluster:** Must be > 0 (invalid if zero)

Rejects:
- Non-FAT12 filesystems
- Corrupted boot sectors
- Unformatted disks

#### 5. Improved Error Messages
```
Mount: FAIL (Check: FAT12? 512B sectors?)
```

Helps diagnose:
- Wrong filesystem type (FAT16/FAT32/NTFS/etc)
- Non-standard sector size
- Unformatted floppy

**Status:** 🔧 SHOULD BE FIXED - Retest required

---

## Testing Instructions (Updated)

### What Changed
1. **Wait now works** - You'll have time to swap floppies
2. **More robust** - Handles drive timing issues
3. **Better errors** - Easier to diagnose problems

### Test Procedure

**Step 1: Write UnoDOS to floppy**
```bash
cd /home/arin/unodos/unodos
make floppy144
sudo ./tools/writeflop.sh -1 /dev/fd0
```

**Step 2: Create test floppy**
Format a floppy as FAT12:
```bash
# Linux
sudo mkfs.vfat -F 12 -n "TEST" /dev/fd0

# Windows
format A: /FS:FAT
```

Add TEST.TXT:
```bash
# Linux
sudo mount /dev/fd0 /mnt
echo "Hello from Omnibook!" | sudo tee /mnt/TEST.TXT
sudo umount /mnt

# Windows
echo Hello from Omnibook! > A:\TEST.TXT
```

**Step 3: Boot Omnibook**
1. Insert UnoDOS floppy
2. Power on or reboot
3. Wait for welcome screen

**Step 4: Run filesystem test**
1. **Press F** (or f) - Test prompt appears
2. **Prompt:** "Insert test floppy (with TEST.TXT) - Press any key"
3. **Swap floppies** - Remove UnoDOS, insert test floppy
4. **Wait 2-3 seconds** - Let drive spin up
5. **Press any key** - Test runs
6. **Results:**
   ```
   Testing...
   Mount: OK
   Open TEST.TXT: OK
   Read: OK - File contents:
   Hello from Omnibook!
   ```

**What to report if it still fails:**

If "Mount: FAIL" still occurs, note:
1. **Floppy brand/type** - Some old floppies are unreliable
2. **Format command used** - Exact mkfs.vfat or format command
3. **Wait time** - How long you waited after swapping
4. **Drive condition** - When was it last cleaned?
5. **Try different floppy** - Some disks just fail

If "Open TEST.TXT: FAIL":
1. Verify filename is exactly "TEST.TXT" (uppercase)
2. Verify file is in root directory (not subfolder)
3. Try mounting on Linux to verify: `sudo mount /dev/fd0 /mnt && ls /mnt`

---

## Technical Details

### clear_kbd_buffer() Implementation
```asm
clear_kbd_buffer:
    push ax
.drain_loop:
    call kbd_getchar        ; Read from keyboard buffer
    test al, al
    jnz .drain_loop         ; Keep draining until empty
.drain_events:
    call event_get_stub     ; Read from event queue
    test al, al
    jnz .drain_events       ; Keep draining until empty
    pop ax
    ret
```

**Why both buffers?**
- INT 09h handler posts to both places
- Application code might use either interface
- Ensures clean state before blocking wait

### Disk Reset Sequence
```asm
; 1. Reset disk system
xor ax, ax              ; AH=00
xor dx, dx              ; DL=0 (drive A:)
int 0x13

; 2. Spin-up delay
mov cx, 0x4000
.spinup:
    nop
    loop .spinup

; 3. Read with retry
mov bp, 3               ; 3 attempts
.retry:
    mov ax, 0x0201      ; Read 1 sector
    mov cx, 0x0001      ; Cyl 0, Sector 1
    mov dx, 0x0000      ; Head 0, Drive 0
    int 0x13
    jnc .success
    dec bp
    jnz .retry
```

**Why multiple attempts?**
- Mechanical drives are timing-sensitive
- First read after swap often fails
- Second/third attempt usually succeeds

### Boot Sector Validation
```asm
; Check signature
cmp word [bpb_buffer + 510], 0xAA55
jne .read_error

; Check bytes per sector
mov ax, [bpb_buffer + 0x0B]
cmp ax, 512
jne .read_error

; Check sectors per cluster
mov al, [bpb_buffer + 0x0D]
test al, al
jz .read_error
```

**Why validate?**
- Prevents parsing garbage data
- Detects wrong filesystem type
- Fails fast with clear error

---

## Known Limitations

These are not bugs, just current implementation limits:

1. **Single cluster reads** - Files larger than 512 bytes read partial
2. **Root directory only** - No subdirectories
3. **8.3 filenames** - No long filename support
4. **Read-only** - Cannot create/modify/delete files
5. **Drive A: only** - Cannot test drive B:

These will be addressed in future versions.

---

## Success Criteria

✅ **Test passes if:**
- Prompt appears after pressing F
- System waits for keypress before reading
- "Mount: OK" appears
- "Open TEST.TXT: OK" appears
- File contents display on screen

❌ **Test fails if:**
- System reads immediately without waiting
- "Mount: FAIL" appears
- "Open TEST.TXT: FAIL" appears
- System hangs or crashes

---

## Next Steps After Successful Test

Once the FAT12 driver is validated on real hardware:

1. **v3.11.0: Application Loader**
   - Load .BIN programs from floppy
   - Execute user applications
   - Return to kernel on exit

2. **v3.12.0: Window Manager**
   - GUI windows with title bars
   - Event routing to applications
   - Multi-window environment

3. **v3.13.0: Multi-Floppy Support**
   - Load drivers from second floppy
   - FAT16/FAT32 driver modules
   - Three-tier architecture active

---

*Document created: 2026-01-23*
*UnoDOS v3.10.0 Patch 1 - Omnibook Hardware Compatibility Fixes*
