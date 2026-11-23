# SmartScreen 提示"已保护你的电脑"怎么办

## 先判断是哪一种

| 现象 | 原因 | 对应处理 |
|---|---|---|
| 提示里写 **「发布者未知」**，文件是本地编译的 | 未签名 + 无信誉 | 方案 B（自签）或点「仍要运行」 |
| 从网上下载/解压出来后提示 | 文件带 **Zone.Identifier**（网络标记） | 方案 A（解除锁定） |
| 换一台电脑又提示 | 自签证书只在本机被信任 | 方案 B 需在新机器重做，或方案 C |

本项目实测：`build\xbgtasix.exe` **没有**网络标记（`Get-Item -Stream *` 只有 `:$DATA`），
所以被拦是"未签名"这一类。

## 方案 A：解除锁定（下载来的文件）

```powershell
Unblock-File .\build\xbgtasix.exe
# 或：右键 -> 属性 -> 勾选「解除锁定」-> 确定
```

只对带网络标记的文件有意义；对本地编译的文件不解决问题。

## 方案 B：本机自签名（消除"发布者未知"）

```powershell
powershell -ExecutionPolicy Bypass -File tools\sign_local.ps1
```

脚本做的事：

1. 在 **CurrentUser** 的「个人」存储建一张自签名 Code Signing 证书（**不需要管理员**）
2. 装进 CurrentUser 的「受信任的根证书颁发机构」+「受信任的发布者」（只影响当前用户）
3. 用它对 `build\xbgtasix.exe` 签名（SHA256）
4. 校验并打印结果

实测输出：

```
已新建证书 F9CEF028277C52BAF7F483C3DE44EE628498553C
  已加入 Root
  已加入 TrustedPublisher
  签名成功（SHA256）
  状态：Valid
  主体：CN=Xbgtasix Local Dev
```

撤销：

```powershell
powershell -ExecutionPolicy Bypass -File tools\sign_local.ps1 -Remove
```

**局限（别误解）**：自签名只在这台机器上有效，因为它靠"本机信任该证书"。
给别人用仍会提示——那需要真正的代码签名证书（EV 证书）+ 信誉积累，属于花钱的事。

## 关键：重新构建会覆盖 exe，签名随之失效

`build.sh` 每次都重写 `build/xbgtasix.exe`——**签名是签在文件上的，文件一变就没了**。
所以：

- 已经装过自签名证书的机器，`build.sh` 结束时会**自动重签**（脚本会打印
  `== 重新签名 xbgtasix.exe`）
- 第一次要先手动装证书：`powershell -ExecutionPolicy Bypass -File tools\sign_local.ps1`

判断一个 exe 到底是不是签名过的：

```powershell
Get-AuthenticodeSignature .\build\xbgtasix.exe | Select-Object Status,@{n='Signer';e={$_.SignerCertificate.Subject}}
```

`Status = Valid` 且主体是 `CN=Xbgtasix Local Dev` 才算签过。

## 先确认你双击的是哪个文件

同一个名字可能有多个副本（旧构建、拷到别处的）。看大小最快：

| 文件 | 大小 |
|---|---|
| 本项目当前构建 | **1,015 KB**（1,038,984 字节） |

如果双击的那个不是这个大小，说明它**不是当前构建**——旧副本没有新签名，
SmartScreen 当然还会拦。用完整路径运行当前构建，或把旧副本删掉。

## 方案 C：不通过 exe 分发

让别人从源码构建（构建产物没有网络标记，也不会被 SmartScreen 当成"下载未知程序"）：

```bash
git clone https://github.com/ljm-233/Xbgtasix && cd Xbgtasix
python tools/fetch_gdk.py --out ../gdk
./build.sh
```

## 方案 D：点「仍要运行」

对话框本身就有这个按钮，放行一次即可。**这是最快的方法**，但每次换文件都要点一次。

## 不建议

关闭 SmartScreen（系统级安全设置），或把整个目录加进排除项。
对本项目没有这个必要——它是本地编译的工具，签名或放行都够用。
