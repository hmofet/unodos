/* Host-side gate for the unoscript fs path helpers (pc64/unoscript_path.c).
 *
 * These four pure functions carry the SECURITY-critical rules of the fs surface:
 * traversal rejection (so a relative path can never leave its home), the exact
 * "USERS/<uid>/" home-name construction, the home-membership test that decides
 * fs.user vs fs.sys, and the absolute "/label/rest" split.  They touch no
 * filesystem and no globals, so they are decidable natively - seconds, no QEMU.
 * The QEMU gate (tools/unoscript_qemu.py) then confirms the wired surface is
 * gated on a real machine.
 *
 * Build + run:  sh tools/unoscript_path_test.sh
 */
#include <stdio.h>
#include <string.h>
#include "unoscript_path.h"

static int fails;
#define OK(cond, msg) do { if (cond) { /* pass */ } else { \
    printf("FAIL %s\n", msg); fails++; } } while (0)

static void t_traversal(void)
{
    /* safe */
    OK(uscp_has_traversal("todo.txt") == 0, "plain name is safe");
    OK(uscp_has_traversal("notes/todo.txt") == 0, "one subdir is safe");
    OK(uscp_has_traversal("a/b/c") == 0, "nested names are safe");
    /* unsafe */
    OK(uscp_has_traversal("") == 1, "empty is unsafe");
    OK(uscp_has_traversal("..") == 1, "'..' is unsafe");
    OK(uscp_has_traversal(".") == 1, "'.' is unsafe");
    OK(uscp_has_traversal("a/../b") == 1, "mid '..' is unsafe");
    OK(uscp_has_traversal("../a") == 1, "leading '..' is unsafe");
    OK(uscp_has_traversal("a/..") == 1, "trailing '..' is unsafe");
    OK(uscp_has_traversal("./x") == 1, "leading '.' is unsafe");
    OK(uscp_has_traversal("a/./b") == 1, "mid '.' is unsafe");
    OK(uscp_has_traversal("a//b") == 1, "empty component ('//') is unsafe");
    OK(uscp_has_traversal("a/") == 1, "trailing slash is unsafe");
    OK(uscp_has_traversal("/x") == 1, "leading slash (empty first comp) is unsafe");
}

static void t_home_name(void)
{
    char b[64];
    OK(uscp_home_name(1, "todo.txt", b, sizeof b) == 16 &&
       strcmp(b, "USERS/1/todo.txt") == 0, "home name for uid 1");
    OK(uscp_home_name(42, "a/b", b, sizeof b) > 0 &&
       strcmp(b, "USERS/42/a/b") == 0, "home name keeps subdirs");
    OK(uscp_home_name(0, "x", b, sizeof b) > 0 &&
       strcmp(b, "USERS/0/x") == 0, "home name for uid 0");
    /* overflow: exact fit needed is 17 bytes ("USERS/1/todo.txt\0"); give 16 */
    OK(uscp_home_name(1, "todo.txt", b, 16) == -1 && b[0] == 0,
       "overflow returns -1 and empties out");
}

static void t_under_home(void)
{
    OK(uscp_under_home(1, "USERS/1/todo.txt") == 1, "own home file is under home");
    OK(uscp_under_home(2, "USERS/2/notes/a") == 1, "own nested home file");
    OK(uscp_under_home(1, "USERS/1/") == 0, "bare home dir (nothing after) is not");
    OK(uscp_under_home(1, "USERS/12/x") == 0, "uid 1 must NOT match uid 12's home");
    OK(uscp_under_home(12, "USERS/1/x") == 0, "uid 12 must NOT match uid 1's home");
    OK(uscp_under_home(1, "USERS/1x/y") == 0, "uid prefix must be exact");
    OK(uscp_under_home(1, "OTHER/1/x") == 0, "different root is not home");
    OK(uscp_under_home(1, "USERS/1") == 0, "missing trailing slash is not under home");
}

static void t_split_abs(void)
{
    char lab[32]; const char *rest = 0;
    OK(uscp_split_abs("/usb/notes/todo.txt", lab, sizeof lab, &rest) == 0 &&
       strcmp(lab, "usb") == 0 && strcmp(rest, "notes/todo.txt") == 0,
       "split /usb/notes/todo.txt");
    OK(uscp_split_abs("/ram/x", lab, sizeof lab, &rest) == 0 &&
       strcmp(lab, "ram") == 0 && strcmp(rest, "x") == 0, "split /ram/x");
    OK(uscp_split_abs("/x", lab, sizeof lab, &rest) == -1, "no rest after label -> fail");
    OK(uscp_split_abs("x", lab, sizeof lab, &rest) == -1, "no leading slash -> fail");
    OK(uscp_split_abs("//x", lab, sizeof lab, &rest) == -1, "empty label -> fail");
    OK(uscp_split_abs("/usb/", lab, sizeof lab, &rest) == -1, "empty rest -> fail");
    OK(uscp_split_abs("/verylonglabelthatexceeds/x", lab, 8, &rest) == -1,
       "label overflow -> fail");
}

int main(void)
{
    t_traversal();
    t_home_name();
    t_under_home();
    t_split_abs();
    if (fails) { printf(">> unoscript_path: %d FAILED\n", fails); return 1; }
    printf(">> unoscript_path host gate OK\n");
    return 0;
}
