# subspawn 手动模块测试

AppSpawnTest 使用 JSON 构造 CREATE、FORK、SHUTDOWN 请求。以下用例需要在设备上手动执行，不会由 AppSpawnModuleTest 自动运行。

## 准备

在 OpenHarmony 源码根目录构建，例如 RK3568：

~~~sh
./build.sh --product-name rk3568 --build-target AppSpawnTest --ccache
~~~

目标为 //base/startup/appspawn/test/moduletest:AppSpawnTest。其余产品使用对应的产品名和 ABI。

将 AppSpawnTest 和本目录的四份 JSON 推送到设备测试目录，并赋予程序执行权限。设备上的 appspawn、subspawn、客户端库及相关模块应为匹配版本。

四份 JSON 使用同一组示例应用身份，运行前统一替换：

- process-name、bundle-info 中的包名和应用索引；
- dac-info 中的 UID、GID 和附加组；app-uid 缺省使用 uid；
- access-token.accessTokenIdEx，使用完整 token 值。
- FORK 的 `ParentUid` 扩展字段，填写同一实例所用的 UID。

请求身份应与目标设备当前应用记录一致。调用进程还须满足服务端连接鉴权和 SELinux 权限，JSON 中的 UID 不代表调用者身份。SHUTDOWN 只接受 SO_PEERCRED 确认的 root 或 foundation 调用者，开发者 shell 与其余连接白名单用户会被拒绝。当前工具通过 cJSON double 读取 token，不能精确表示的数值需要先解决转换精度。

## 执行

以下命令在设备测试目录中执行。必须保留 --mode subspawn，避免普通模式自动添加 IGNORE_SANDBOX。

第一次创建实例：

~~~sh
./AppSpawnTest --mode subspawn --once --file subspawn_create.json --state state.json
~~~

CREATE 成功后，state.json 保存实例 PID 和 socket 路径。若已有相同身份的存活实例，CREATE 会复用它。

在实例中创建子进程：

~~~sh
./AppSpawnTest --mode subspawn --once --file subspawn_fork.json --state state.json
~~~

FORK 用例包含 APP_FLAGS_CHILDPROCESS。该命令验证请求和子进程创建；AppSpawnTest 不会在 AbilityManagerService 中创建 ChildProcessRecord，完整业务入口需经 AbilityManagerService 子进程接口验证。

关闭实例：

~~~sh
./AppSpawnTest --mode subspawn --once --file subspawn_shutdown.json --state state.json
~~~

SHUTDOWN 发给主 appspawn，工具根据 state.json 自动附加 SubspawnPid。主 appspawn 通过 `KillAndWaitStatus(pid, SIGTERM, ...)` 等待实例退出，等待失败则返回错误。成功后仍需验证业务子进程、socket 和 cgroup 的实际清理结果。

验证非法预加载 JSON 时，应先关闭该应用的已有实例，避免复用绕过预加载：

~~~sh
./AppSpawnTest --mode subspawn --once --file subspawn_create_preload_invalid.json --state state_invalid.json
~~~

应命中 APPSPAWN_SUBSPAWN_PRELOAD_FAILED，不能仅凭工具非零退出判断用例通过。

## 预加载配置

`subspawn_create.json` 已提供 ARM 版 `libentry.so` 的预加载配置。使用 ARM64 测试设备时，按已安装 HAP 的实际布局调整，例如：

~~~json
{
  "name": "SubspawnPreload",
  "value": "[{\"libPath\":[\"/data/storage/el1/bundle/libs/arm64\"],\"so\":[\"/data/storage/el1/bundle/libs/arm64/libentry.so\"],\"ns\":\"default\"}]"
}
~~~

库路径使用应用沙箱视图，按实际 nativeLibraryPath 和 so 名称填写；RK3568 的 arm 应用通常使用 libs/arm。修改配置后需关闭旧实例，再验证第一次 CREATE。

验证时同时检查传输返回值和服务端 result。成功均为 0。发起 FORK 前检查实例 maps 中是否已有目标 so。JSON 工具只验证 FORK 响应和返回的业务 PID，子进程可能很快退出。使用真实 HAP 验证子进程进入业务入口和继承映射。若出现读取 JSON 失败后使用默认配置的日志，应先修正配置，再重新执行。

--once 将请求失败映射为退出码 1，成功为 0。state.json 可能是上次运行留下的文件，应在确认本次 CREATE 成功后使用。

## MST 用例（AppSpawn_Subspawn_MST）

`test/moduletest/subspawn/subspawn_mst_test.cpp` 提供 3 条自动用例。预加载仍按上面的 JSON 流程验证。

| 用例 | 验证点 |
| --- | --- |
| `Subspawn_Mst_Create_001` | 实例进程存活、socket 属主正确，同一应用再次 CREATE 返回相同 PID |
| `Subspawn_Mst_Fork_001` | 连接实例 socket 发起 FORK，检查成功响应及独立的业务 PID，关闭实例后检查业务进程回收 |
| `Subspawn_Mst_Shutdown_001` | 经主 appspawn 关闭实例；实例进程退出且 socket 被清理 |

运行前提：

1. 子功能开关：`param set const.startup.appspawn_support_subspawn.enable 1`（产品未预置时需要）。
2. 使用设备上已安装的专用测试 HAP，并确保对应 UID 没有现存 subspawn 实例。通过 `bm dump -n <bundle>` 核对包名、UID 和完整的 `accessTokenIdEx`，将同一应用的身份传入测试进程。

   ~~~sh
   SUBSPAWN_MST_BUNDLE="$bundle" SUBSPAWN_MST_UID="$uid" \
     SUBSPAWN_MST_TOKEN="$accessTokenIdEx" ./AppSpawn_Subspawn_MST
   ~~~

   `SUBSPAWN_MST_GID` 默认使用 UID，`SUBSPAWN_MST_APP_INDEX` 默认使用 0。按应用实际信息填写可选项。未提供 token 或功能未开启时用例自动跳过。
3. debug/userdebug 镜像：root 测试进程依赖 `permissive su`（`su.te:14`），user 版本上会因 SELinux 拒绝而失败，属既有基线前提。
4. CREATE 还要求 isolate sandbox：用例已设置 `APP_FLAGS_ISOLATED_SANDBOX_TYPE` 并关闭 `NO_SANDBOX`/`IGNORE_SANDBOX`。
