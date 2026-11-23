# 手柄支持：两条原生路径 + 一层需要兼容层的关联

## 结论先说

**手柄读取不需要兼容层。** PC 上两条路径都是操作系统提供的，我们只需验证它们可用，
并补上 GDK 里唯一"确实需要我们自己实现"的那一层：**设备与用户的关联**。

| 路径 | 谁提供 | 实测 |
|---|---|---|
| `XInput1_4.dll` | Windows 自带 | 读到 1 个手柄（槽 0），`XInputGetState` 正常 |
| `GameInput.dll` | Windows 自带 | `GameInputCreate` 成功；`RegisterDeviceCallback` 枚举到 `vid=0x045E pid=0x028E`（Xbox 360 手柄） |
| GDK 的 `GameInput.lib` | NuGet 包里的 GDK | 直接链系统 `GameInput.dll`，**无版本冲突** |

也就是说：GDK 标题在 PC 上调用 `GameInputCreate` 时，走的是系统实现，我们不用转发。

## 需要兼容层参与的部分

GDK 多一层"哪个手柄属于哪个用户"：

```c
XUserFindForDevice(const APP_LOCAL_DEVICE_ID* deviceId, XUserHandle* handle);
XUserRegisterForDeviceAssociationChanged(queue, ctx, callback, &token);
XUserUnregisterForDeviceAssociationChanged(token, wait);
```

主机上这由系统维护（多人分屏、手柄热插拔、按 Xbox 键换人）。PC 上我们只有一个本地用户，
所以语义定为：

| 输入 | 行为 |
|---|---|
| 全零设备 ID（`XUserNullDeviceId`） | `E_INVALIDARG`——"没有这台设备" |
| 任何非零设备 ID | 返回本地用户句柄 |
| 关联变化事件 | **永不触发**（PC 上用户不会切） |
| 注册 | 必须给出非零 token |
| 注销 | 返回 `true`（与仓内其他 Unregister 一致） |

**为什么"永不触发"是对的**：如果我们在 PC 上伪造"手柄换人"事件，
游戏会去重建本地用户、重置输入上下文，而实际上什么都没变——那才是真正的 bug。

## 已知限制

| 限制 | 原因 |
|---|---|
| 只有单本地用户 | 没有 XBL 登录，多人分屏用不了 |
| 手柄→用户是 1:1 恒等映射 | 主机上靠系统 UI 选择用户，PC 上没有等价入口 |
| 不处理 Xbox 键（Guide） | 那是系统层面的事，`GameInput` 的 Guide 回调在 PC 上有系统占用 |

## 复现

```bash
./build/t35_gamepad.exe      # 需要接着手柄；没接的话 XInput/GameInput 各自 SKIP
```

实测输出（本机接着 Xbox 360 手柄）：

```
[t35] XInput 槽 0: 按键=0x0000 摇杆L=(0,0) 扳机=(0,0)
[t35] XInput 找到 1 个手柄
[t35] GameInput 设备: vid=0x045E pid=0x028E status=0x1
[t35] 枚举到手柄设备 1 个
[t35] FindForDevice(有效 ID) 返回本地用户
```
