# TurboDB EU Linux Docker 远程测试 Runbook

本文用于把 Windows 当前工作树作为唯一源码事实源，上传到 `root@eu`，在一次性 Ubuntu 24.04 Docker 容器内构建 TurboUtils 与 TurboDB，并保存可复验的构建、测试和校验结果。

该流程默认测试纯 C ORM、Redis/CFlow、Mongo、PostgreSQL 适配层及仓库内 mock/contract tests。它关闭独立 TidesDB engine tests，但仍保留 TurboDB 自身的 TidesDB adapter/mock tests。除非另有测试任务，本流程不会连接或修改远端现有数据库服务。

## 1. 测试契约

- 源码事实源是本机 `C:\projects\cpp\turbonet` 下的当前工作树，包括尚未提交的改动。
- 归档只包含 `turbo-utils` 与 `turbodb`；排除 Git 元数据、构建目录、依赖安装目录、CodeGraph 索引、日志与 `.env` 文件。
- 每次执行使用独立的 `/root/dev/runs/<run-id>` 和同名 Docker 容器。
- 容器使用 `--network host`，但测试默认仅启动仓库内测试进程，不管理远端已有容器或数据库。
- 所有步骤 fail fast；不得在 epoll 初始化失败时自动改用其他 CFlow I/O backend。
- Docker 容器在成功、失败或中断后都按精确名称清理；源码、日志和 JUnit 结果保留在 run 目录。

## 2. 前置检查

本机 PowerShell 需要 `git`、`tar.exe`、`ssh` 和 `scp`。远端需要 Docker，以及已经安装的 `/opt/vcpkg/vcpkg`。

在本机执行：

```powershell
$sourceRoot = 'C:\projects\cpp\turbonet'
$turboUtilsRoot = Join-Path $sourceRoot 'turbo-utils'
$turboDbRoot = Join-Path $sourceRoot 'turbodb'

Test-Path (Join-Path $turboUtilsRoot 'CMakeUserPresets.json')
Test-Path (Join-Path $turboDbRoot 'CMakeUserPresets.json')
ssh root@eu 'set -eu; docker version --format "{{.Server.Version}}"; test -x /opt/vcpkg/vcpkg; mkdir -p /root/dev/incoming /root/dev/runs /var/cache/vcpkg'
```

两个 `Test-Path` 都必须输出 `True`，远端命令必须成功。

## 3. 打包当前工作树

在本机 PowerShell 执行：

```powershell
$sourceRoot = 'C:\projects\cpp\turbonet'
$artifactRoot = 'C:\projects\cpp\artifacts'
$stamp = Get-Date -Format 'yyyyMMddTHHmmss'
$bundleName = "turbodb-eu-$stamp.zip"
$bundlePath = Join-Path $artifactRoot $bundleName
$shaPath = "$bundlePath.sha256"
$manifestName = "$bundleName.revisions.txt"
$manifestPath = Join-Path $artifactRoot $manifestName

New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

@(
    "created_utc=$([DateTime]::UtcNow.ToString('o'))"
    "turbo-utils_commit=$(git -C (Join-Path $sourceRoot 'turbo-utils') rev-parse HEAD)"
    "turbo-utils_dirty=$((git -C (Join-Path $sourceRoot 'turbo-utils') status --porcelain | Measure-Object).Count)"
    "turbodb_commit=$(git -C (Join-Path $sourceRoot 'turbodb') rev-parse HEAD)"
    "turbodb_dirty=$((git -C (Join-Path $sourceRoot 'turbodb') status --porcelain | Measure-Object).Count)"
) | Set-Content -Encoding ascii -LiteralPath $manifestPath

tar.exe -a -cf $bundlePath `
    --exclude='.git' `
    --exclude='.codegraph' `
    --exclude='.worktrees' `
    --exclude='build' `
    --exclude='vcpkg_installed' `
    --exclude='.env' `
    --exclude='.env.*' `
    --exclude='*.log' `
    -C $sourceRoot turbo-utils turbodb `
    -C $artifactRoot $manifestName

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $bundlePath).Hash.ToLowerInvariant()
"$hash  $bundleName" | Set-Content -Encoding ascii -LiteralPath $shaPath

tar.exe -tf $bundlePath | Select-String -Pattern '(^|/)\.git/|(^|/)\.env($|\.)|(^|/)build/'
Get-Content -LiteralPath $shaPath
```

`Select-String` 不应输出任何内容。若它发现被排除的路径，停止上传并检查归档参数。

## 4. 上传并校验源码包

继续在同一个 PowerShell 会话执行：

```powershell
scp $bundlePath $shaPath $manifestPath root@eu:/root/dev/incoming/
ssh root@eu "cd /root/dev/incoming && sha256sum -c '$bundleName.sha256' && printf '%s\n' '$bundleName' > latest-turbodb-bundle"
```

必须看到归档的 `OK`。校验失败时不要解压或复用旧归档。

## 5. 在 EU Docker 中构建和测试

建议在远端 `tmux` 会话内执行，以免本地网络断开终止测试：

```powershell
ssh -t root@eu 'tmux new-session -A -s turbodb-eu'
```

进入远端后执行以下 Bash 脚本：

```bash
set -Eeuo pipefail

incoming=/root/dev/incoming
bundle_name="$(cat "$incoming/latest-turbodb-bundle")"
case "$bundle_name" in
    turbodb-eu-*.zip) ;;
    *) echo "invalid bundle marker: $bundle_name" >&2; exit 2 ;;
esac

cd "$incoming"
sha256sum -c "$bundle_name.sha256"

run_id="turbodb-eu-$(date -u +%Y%m%dT%H%M%SZ)"
run_root="/root/dev/runs/$run_id"
container_name="$run_id"
mkdir -p "$run_root/src" "$run_root/artifacts"
unzip -q "$incoming/$bundle_name" -d "$run_root/src"
cp "$incoming/$bundle_name.sha256" "$run_root/artifacts/source.sha256"
cp "$incoming/$bundle_name.revisions.txt" "$run_root/artifacts/source.revisions.txt"
printf '%s\n' "$run_root" > "$incoming/latest-turbodb-run"

test -f "$run_root/src/turbo-utils/CMakeUserPresets.json"
test -f "$run_root/src/turbodb/CMakeUserPresets.json"
test -x /opt/vcpkg/vcpkg

cleanup_container() {
    docker rm -f "$container_name" >/dev/null 2>&1 || true
}
trap cleanup_container EXIT INT TERM
cleanup_container

docker pull ubuntu:24.04
docker image inspect --format '{{.Id}}' ubuntu:24.04 > "$run_root/artifacts/container-image.txt"

docker run --name "$container_name" \
    --network host \
    --mount "type=bind,src=$run_root,dst=/work" \
    --mount "type=bind,src=/opt/vcpkg,dst=/opt/vcpkg" \
    --mount "type=bind,src=/var/cache/vcpkg,dst=/var/cache/vcpkg" \
    -e VCPKG_ROOT=/opt/vcpkg \
    -e VCPKG_DEFAULT_BINARY_CACHE=/var/cache/vcpkg \
    -e DEBIAN_FRONTEND=noninteractive \
    ubuntu:24.04 bash -lc '
set -Eeuo pipefail

apt-get update
apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake curl git ninja-build \
    pkg-config python3-minimal re2c unzip zip

{
    date -u --iso-8601=seconds
    uname -a
    gcc --version
    cmake --version
    ninja --version
    re2c --version
    /opt/vcpkg/vcpkg version
} | tee /work/artifacts/environment.txt

cd /work/src/turbo-utils
cmake --fresh --preset linux-release-user \
    -DTURBO_ENABLE_EPOLL_READINESS=ON \
    -DENABLE_TESTS=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_TESTING=OFF
cmake --build --preset linux-release-user
cmake --build --preset install-linux-release-user

cd /work/src/turbodb
cmake --fresh --preset linux-release-user \
    -DTIDESDB_BUILD_TESTS=OFF \
    -DORM_WITH_TIDESDB=OFF \
    -DORM_WITH_REDIS=ON \
    -DENABLE_TESTS=ON \
    -DBUILD_TESTING=ON
cmake --build --preset linux-release-user
ctest --preset linux-release-user \
    --timeout 60 \
    --output-on-failure \
    --output-junit /work/artifacts/turbodb-linux-release.xml

python3 - <<"PY"
import xml.etree.ElementTree as ET

path = "/work/artifacts/turbodb-linux-release.xml"
root = ET.parse(path).getroot()
suites = [root] if root.tag == "testsuite" else list(root.findall("testsuite"))
tests = sum(int(s.get("tests", "0")) for s in suites)
failures = sum(int(s.get("failures", "0")) for s in suites)
errors = sum(int(s.get("errors", "0")) for s in suites)
skipped = sum(int(s.get("skipped", "0")) for s in suites)
if tests <= 0 or failures != 0 or errors != 0 or skipped != 0:
    raise SystemExit(
        f"invalid JUnit result: tests={tests} failures={failures} "
        f"errors={errors} skipped={skipped}"
    )
print(
    f"JUnit verified: tests={tests} failures={failures} "
    f"errors={errors} skipped={skipped}"
)
PY
' 2>&1 | tee "$run_root/artifacts/linux-build-test.log"

cleanup_container

sha256sum \
    "$run_root/artifacts/source.sha256" \
    "$run_root/artifacts/source.revisions.txt" \
    "$run_root/artifacts/container-image.txt" \
    "$run_root/artifacts/environment.txt" \
    "$run_root/artifacts/linux-build-test.log" \
    "$run_root/artifacts/turbodb-linux-release.xml" \
    > "$run_root/artifacts/SHA256SUMS"

test "$(docker ps -aq --filter "name=^/${container_name}$" | wc -l)" -eq 0

result_name="$run_id-results.zip"
cd "$run_root"
zip -qr "$incoming/$result_name" artifacts
cd "$incoming"
sha256sum "$result_name" > "$result_name.sha256"
printf '%s\n' "$result_name" > latest-turbodb-result

echo "run_root=$run_root"
echo "result=$incoming/$result_name"
cat "$incoming/$result_name.sha256"
```

`cmake --fresh` 保证每次使用新的配置事实，避免旧 cache 掩盖 feature 选项变化。`ctest --timeout 60` 为每个测试设置上限，防止网络 contract test 在初始化提前失败后无限等待。

## 6. 下载并复验结果

在本机新的 PowerShell 会话执行：

```powershell
$artifactRoot = 'C:\projects\cpp\artifacts'
$resultName = (ssh root@eu 'cat /root/dev/incoming/latest-turbodb-result').Trim()
if ($resultName -notmatch '^turbodb-eu-[0-9TZ]+-results\.zip$') {
    throw "Unexpected result marker: $resultName"
}

scp "root@eu:/root/dev/incoming/$resultName" $artifactRoot
scp "root@eu:/root/dev/incoming/$resultName.sha256" $artifactRoot

$localResult = Join-Path $artifactRoot $resultName
$expected = ((Get-Content -LiteralPath "$localResult.sha256") -split '\s+')[0].ToLowerInvariant()
$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $localResult).Hash.ToLowerInvariant()
if ($actual -ne $expected) {
    throw "Result checksum mismatch: expected=$expected actual=$actual"
}

tar.exe -tf $localResult
```

解压后至少检查：

- `source.sha256` 与上传源码包一致。
- `source.revisions.txt` 记录了两个仓库的 commit 与 dirty file 数量。
- `environment.txt` 中包含实际 GCC、CMake、Ninja、re2c 与 vcpkg 版本。
- `linux-build-test.log` 最终包含 CTest 全部通过摘要和 `JUnit verified`。
- `turbodb-linux-release.xml` 中测试数大于零，且 `failures`、`errors`、`skipped` 都为零。
- `SHA256SUMS` 能复验全部证据文件。

## 7. 成功判定

只有同时满足以下条件，才能声明本次 EU 远程测试通过：

1. 上传前与远端解压前的源码包 SHA-256 校验均通过。
2. TurboUtils 以 `TURBO_ENABLE_EPOLL_READINESS=ON` 成功构建并安装。
3. TurboDB 成功 configure/build，且独立 TidesDB engine tests 被关闭。
4. CTest 实际发现至少一个测试，JUnit 的 failure、error 与 skipped 数均为零。
5. Redis/CFlow 与 ORM contract tests 在同一次 run 内通过。
6. 精确命名的测试容器已被删除，run 目录和证据文件仍保留。

## 8. 常见失败

### `re2c` 不存在

TurboUtils 的生成步骤需要 `re2c`。确认 Ubuntu 容器安装命令包含 `re2c`，然后创建新的 run；不要复用已经部分配置的 build tree。

### Redis runtime 返回 `TURBO_ENOTSUP (-4039)`

这通常表示 TurboUtils 未启用 Linux epoll readiness。确认 TurboUtils configure 命令含有：

```text
-DTURBO_ENABLE_EPOLL_READINESS=ON
```

随后以新的 run 重新构建 TurboUtils 和 TurboDB。不得在 TurboDB 内加入静默 backend fallback，因为这会改变 CFlow backend 选择和错误语义。

### Redis contract test 等待超时

先检查日志中是否有 runtime 初始化或连接失败。初始化失败可能使测试内 fake server 等不到连接；`ctest --timeout 60` 只负责有界退出，不能替代根因修复。

### 仍看到 TidesDB 测试

`TIDESDB_BUILD_TESTS=OFF` 与 `ORM_WITH_TIDESDB=OFF` 关闭独立 TidesDB engine 集成测试。TurboDB 自身不依赖真实 TidesDB 的 adapter/mock tests 仍可能出现，这是预期行为。

### vcpkg 下载慢或重复编译

确认 `/var/cache/vcpkg` 在远端存在，并以 bind mount 映射到容器同一路径；同时保留 `VCPKG_DEFAULT_BINARY_CACHE=/var/cache/vcpkg`。不要把某个 run 的 `vcpkg_installed` 目录复制到另一个 run。

## 9. 清理策略

默认只清理本次精确命名的 Docker 容器，不删除 run 目录或上传包。需要释放空间时，先打印并人工核对绝对路径，然后只删除明确指定的单个历史 run；不得用通配符递归清理 `/root/dev/runs` 或 `/root/dev/incoming`。
