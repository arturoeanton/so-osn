/* This is a generated file, don't edit */

#define NUM_APPLETS 80
#define KNOWN_APPNAME_OFFSETS 4

const uint16_t applet_nameofs[] ALIGN2 = {
89,
210,
324,
};

const char applet_names[] ALIGN1 = ""
"[" "\0"
"[[" "\0"
"ash" "\0"
"awk" "\0"
"base64" "\0"
"basename" "\0"
"bc" "\0"
"cat" "\0"
"cksum" "\0"
"cp" "\0"
"cpio" "\0"
"cut" "\0"
"date" "\0"
"dc" "\0"
"dd" "\0"
"df" "\0"
"diff" "\0"
"dirname" "\0"
"du" "\0"
"echo" "\0"
"env" "\0"
"envdir" "\0"
"envuidgid" "\0"
"expand" "\0"
"factor" "\0"
"false" "\0"
"find" "\0"
"fold" "\0"
"free" "\0"
"grep" "\0"
"head" "\0"
"hexdump" "\0"
"kill" "\0"
"killall" "\0"
"ls" "\0"
"lsattr" "\0"
"lsmod" "\0"
"lsof" "\0"
"lspci" "\0"
"lsscsi" "\0"
"lsusb" "\0"
"md5sum" "\0"
"mkdir" "\0"
"more" "\0"
"mv" "\0"
"patch" "\0"
"printf" "\0"
"ps" "\0"
"pwd" "\0"
"pwdx" "\0"
"readlink" "\0"
"realpath" "\0"
"rev" "\0"
"rm" "\0"
"rmdir" "\0"
"rmmod" "\0"
"sed" "\0"
"sh" "\0"
"sha1sum" "\0"
"sha256sum" "\0"
"sleep" "\0"
"sort" "\0"
"stat" "\0"
"strings" "\0"
"tac" "\0"
"tail" "\0"
"test" "\0"
"timeout" "\0"
"top" "\0"
"touch" "\0"
"tr" "\0"
"traceroute" "\0"
"tree" "\0"
"true" "\0"
"truncate" "\0"
"uniq" "\0"
"uptime" "\0"
"vi" "\0"
"wc" "\0"
"xargs" "\0"
;

#define APPLET_NO_ash 2
#define APPLET_NO_awk 3
#define APPLET_NO_base64 4
#define APPLET_NO_basename 5
#define APPLET_NO_bc 6
#define APPLET_NO_cat 7
#define APPLET_NO_cksum 8
#define APPLET_NO_cp 9
#define APPLET_NO_cpio 10
#define APPLET_NO_cut 11
#define APPLET_NO_date 12
#define APPLET_NO_dc 13
#define APPLET_NO_dd 14
#define APPLET_NO_df 15
#define APPLET_NO_diff 16
#define APPLET_NO_dirname 17
#define APPLET_NO_du 18
#define APPLET_NO_echo 19
#define APPLET_NO_env 20
#define APPLET_NO_envdir 21
#define APPLET_NO_envuidgid 22
#define APPLET_NO_expand 23
#define APPLET_NO_factor 24
#define APPLET_NO_false 25
#define APPLET_NO_find 26
#define APPLET_NO_fold 27
#define APPLET_NO_free 28
#define APPLET_NO_grep 29
#define APPLET_NO_head 30
#define APPLET_NO_hexdump 31
#define APPLET_NO_kill 32
#define APPLET_NO_killall 33
#define APPLET_NO_ls 34
#define APPLET_NO_lsattr 35
#define APPLET_NO_lsmod 36
#define APPLET_NO_lsof 37
#define APPLET_NO_lspci 38
#define APPLET_NO_lsscsi 39
#define APPLET_NO_lsusb 40
#define APPLET_NO_md5sum 41
#define APPLET_NO_mkdir 42
#define APPLET_NO_more 43
#define APPLET_NO_mv 44
#define APPLET_NO_patch 45
#define APPLET_NO_printf 46
#define APPLET_NO_ps 47
#define APPLET_NO_pwd 48
#define APPLET_NO_pwdx 49
#define APPLET_NO_readlink 50
#define APPLET_NO_realpath 51
#define APPLET_NO_rev 52
#define APPLET_NO_rm 53
#define APPLET_NO_rmdir 54
#define APPLET_NO_rmmod 55
#define APPLET_NO_sed 56
#define APPLET_NO_sh 57
#define APPLET_NO_sha1sum 58
#define APPLET_NO_sha256sum 59
#define APPLET_NO_sleep 60
#define APPLET_NO_sort 61
#define APPLET_NO_stat 62
#define APPLET_NO_strings 63
#define APPLET_NO_tac 64
#define APPLET_NO_tail 65
#define APPLET_NO_test 66
#define APPLET_NO_timeout 67
#define APPLET_NO_top 68
#define APPLET_NO_touch 69
#define APPLET_NO_tr 70
#define APPLET_NO_traceroute 71
#define APPLET_NO_tree 72
#define APPLET_NO_true 73
#define APPLET_NO_truncate 74
#define APPLET_NO_uniq 75
#define APPLET_NO_uptime 76
#define APPLET_NO_vi 77
#define APPLET_NO_wc 78
#define APPLET_NO_xargs 79

#ifndef SKIP_applet_main
int (*const applet_main[])(int argc, char **argv) = {
test_main,
test_main,
ash_main,
awk_main,
baseNUM_main,
basename_main,
bc_main,
cat_main,
cksum_main,
cp_main,
cpio_main,
cut_main,
date_main,
dc_main,
dd_main,
df_main,
diff_main,
dirname_main,
du_main,
echo_main,
env_main,
chpst_main,
chpst_main,
expand_main,
factor_main,
false_main,
find_main,
fold_main,
free_main,
grep_main,
head_main,
hexdump_main,
kill_main,
kill_main,
ls_main,
lsattr_main,
lsmod_main,
lsof_main,
lspci_main,
lsscsi_main,
lsusb_main,
md5_sha1_sum_main,
mkdir_main,
more_main,
mv_main,
patch_main,
printf_main,
ps_main,
pwd_main,
pwdx_main,
readlink_main,
realpath_main,
rev_main,
rm_main,
rmdir_main,
rmmod_main,
sed_main,
ash_main,
md5_sha1_sum_main,
md5_sha1_sum_main,
sleep_main,
sort_main,
stat_main,
strings_main,
tac_main,
tail_main,
test_main,
timeout_main,
top_main,
touch_main,
tr_main,
traceroute_main,
tree_main,
true_main,
truncate_main,
uniq_main,
uptime_main,
vi_main,
wc_main,
xargs_main,
};
#endif

