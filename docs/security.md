# 権限とセットアップ

これまで全部 root で動いていた。開発中はそれで速いが、
OS としては成立していないので直した。

## 誰で動いているか

```
myos-init (PID 1)                 root
  └ xinit -> X                    root (ハードウェアを触るので)
      └ /myos-session             root  … セットアップとログオンだけ
          └ su - <user>
              └ myos-desktop      一般ユーザー
                  └ myos-wm       一般ユーザー
                      └ Firefox / Java / ファイルマネージャ …
```

デスクトップから先は全部一般ユーザー。
root が要る操作は、そのつど `myos-runas` で昇格する。

### root には直接ログインできない

```sh
passwd -l root
```

`/etc/shadow` の root の欄は `!` 始まりになる。
`myos-login` は `!` や `*` で始まるハッシュ、空のハッシュを必ず弾くので、
どんな入力でも root では入れない。

管理者の仕事は「最初のユーザー + sudo」でやる。Windows で
Administrator を無効にして、普段使いのアカウントを管理者にするのと同じ形。

### 誰が管理者か

初回セットアップで作る最初のユーザーが管理者になる。
実体は `sudo` グループへの所属。

```
%sudo ALL=(ALL:ALL) ALL
%sudo ALL=(ALL) NOPASSWD: /sbin/poweroff, /sbin/reboot, /sbin/halt
```

`NOPASSWD` は電源まわりだけに絞ってある。
目の前の機械の電源を落とすのに認証を求めても意味が無く、
そこから出来ることも「切る」しかないため。
それ以外は必ずパスワードを聞く。

## 管理者として実行 (`src/gui/myos_runas.c`)

Windows の UAC にあたるもの。Win98 風の枠でパスワードを聞き、
`sudo` に渡してコマンドを起動する。

- ファイルマネージャの右クリック →「Run as administrator...」
- 権限が要るコマンドを自分で叩くとき

### 判定は自分でやらない

**「このユーザーは管理者か」「パスワードは合っているか」を
自前で判定しない。** 全部 `sudo` (PAM) に任せて、結果だけ見る。

自作の判定を挟むと、そこがそのまま穴になる。
`sudo` グループに居ない人は、正しいパスワードを入れても
`sudo` 側で弾かれる。それでいい。

### パスワードの扱い

```c
execvp("sudo", argv);          /* 子: -S で標準入力から読ませる */
...
write(fd[1], pw.buf, pw.len);  /* 親: パイプに流す */
memset(pw.buf, 0, sizeof(pw.buf));
```

- **コマンドラインには絶対に載せない。** `ps` で誰からでも見える
- 渡し終えたらすぐ `memset` で潰す
- `sudo -k` を付けて、前回の認証を使い回さない

## 初回セットアップ (`src/gui/myos_setup.c`)

`/etc/myos/setup-done` が無いときだけ、デスクトップより先に出る。
青い背景に灰色のウィザードが乗った、あの画面。

| 手順 | 決めること |
| --- | --- |
| 1 | ようこそ |
| 2 | ユーザー名（この人が管理者になる） |
| 3 | パスワード（+ 確認） |
| 4 | 自動ログインの有無 / キーボード配列 / タイムゾーン |
| 5 | 確認して適用 |

適用でやること:

```sh
useradd -m -s /bin/bash -G sudo,audio,video,plugdev,cdrom,dialout <user>
chpasswd                       # パスワードは標準入力から
passwd -l root
ln -sf /usr/share/zoneinfo/<tz> /etc/localtime
# /etc/default/keyboard と /etc/X11/xorg.conf.d/10-keyboard.conf
# /etc/myos/login.conf に user と autologin
```

ウィンドウマネージャがまだ居ないので、`override_redirect` の
全画面ウィンドウにして、自分で `XSetInputFocus` している。

出口は **Shut down** の釦だけ (もとは Cancel)。押すと終了状態 2 で返り、
`myos-session` が `myos-poweroff` を呼んで電源を落とす。ESC では抜けない
(事故で押せる場所に、代償の重い出口を置かない)。

`setup-done` は最後まで終わったときにしか置かないので、Shut down で
抜けた機械は次の起動でまたここから始まる。

セットアップが途中で死んだ (終了状態が 0 でも 2 でもない) ときだけ、
root のままデスクトップを出す。何も出来ないより、直せる画面が出るほうがいい。

## ログオン画面 (`src/gui/myos_login.c`)

自動ログインを切ったときだけ出る。
通ったらユーザー名を標準出力に 1 行書いて終わり、
セッションのスクリプトがそれを受け取る。

照合は `/etc/shadow` のハッシュを `crypt_r()` に同じ塩で掛け直して比べるだけ。
PAM は使っていない。X から使うには話が大きすぎるのと、
ここで要るのは「本人か」の一点だけなので。
`/etc/shadow` は root しか読めないため、この画面も root で動く。

## X に一般ユーザーから繋ぐ

X サーバーは root が上げる（フレームバッファを触るため）が、
その先は一般ユーザーに落とす。そのままだと
`No protocol specified` で繋がらないので、2 つ手を打っている。

```sh
xhost "+si:localuser:$user"          # サーバー側で許可する
cp "$XAUTHORITY" "$home/.Xauthority" # 認証クッキーも渡す
chown "$user" "$home/.Xauthority"
```

`si:localuser` は X サーバーが接続元のローカルユーザーを見て判断する仕組み。
クッキーのコピーはその保険。

あわせて `xserver-xorg-legacy` を入れ、
`/etc/X11/Xwrapper.config` に `allowed_users=anybody` を書いてある。
これが無いと `Xorg.wrap` に弾かれる。

## 設定ファイルの置き場所 (`src/gui/myosconf.h`)

```
/etc/myos/<name>      システム既定。root しか書けない
~/.myos/<name>        ユーザーの上書き。本人が書ける。こちらが優先
```

読むときは `myos_conf()`、書くときは `myos_user_conf()`。

**壁紙や配色、関連付けを変えるのに管理者権限が要らないのはこのため。**
逆に、全員に効かせたいものは `/etc/myos` を触るので昇格が要る。

対象は `theme.conf` / `desktop.conf` / `filetypes.conf` / `java.conf`。

## リムーバブルメディアの所有者

FAT / exFAT / NTFS には Unix の所有者という考えが無い。
そのままマウントすると root のものになり、
一般ユーザーのデスクトップから書けなくなる。

```sh
mount -o rw,noatime,uid=$uid,gid=$gid,umask=0022 /dev/sdb1 /media/sdb1
```

`uid=` を受け付けない ext4 などは、その場合だけ従来どおりマウントし直す。
`uid` は `/etc/myos/login.conf` の `user` から引いている。
