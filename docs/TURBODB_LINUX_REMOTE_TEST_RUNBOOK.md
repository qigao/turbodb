# TurboDB EU Linux Docker 远程测试 Runbook

本文用于把 Windows 当前工作树作为唯一源码事实源，上传到 `root@eu`，在一次性 Ubuntu 24.04 Docker 容器内构建 TurboUtils 与 TurboDB，并保存可复验的构建、测试和校验结果。

该流程默认测试纯 C ORM、standalone schema tools、Redis/CFlow、Mongo、PostgreSQL 适配层及仓库内 mock/contract tests。它关闭独立 TidesDB engine tests，但仍保留 TurboDB 自身的 TidesDB adapter/mock tests。除非另有测试任务，本流程不会连接或修改远端现有数据库服务。

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
    --exclude='vcpkg_installed_pg' `
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

进入远端后执行以下 Bash 脚本。默认不连接真实 PostgreSQL；需要把
PostgreSQL live test 纳入同一次全量 CTest 时，先执行
`export TURBODB_EU_POSTGRES_LIVE=1`。该模式只创建本次 run 专属的
`postgres:17.6-alpine3.22` 容器和隔离测试表，不访问已有数据库：

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
postgres_name="$run_id-postgres"
enable_postgres_live="${TURBODB_EU_POSTGRES_LIVE:-0}"
case "$enable_postgres_live" in
    0|1) ;;
    *) echo "TURBODB_EU_POSTGRES_LIVE must be 0 or 1" >&2; exit 2 ;;
esac
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
    docker rm -f "$postgres_name" >/dev/null 2>&1 || true
}
trap cleanup_container EXIT INT TERM
cleanup_container

docker pull ubuntu:24.04
docker image inspect --format '{{.Id}}' ubuntu:24.04 > "$run_root/artifacts/container-image.txt"

postgres_conninfo=""
if [ "$enable_postgres_live" = 1 ]; then
    docker pull postgres:17.6-alpine3.22
    docker image inspect --format '{{.Id}}' postgres:17.6-alpine3.22 \
        >> "$run_root/artifacts/container-image.txt"
    docker run -d --name "$postgres_name" \
        --tmpfs /var/lib/postgresql/data:rw,noexec,nosuid \
        -e POSTGRES_USER=turbodb \
        -e POSTGRES_PASSWORD=turbodb \
        -e POSTGRES_DB=turbodb \
        -p 127.0.0.1::5432 \
        postgres:17.6-alpine3.22 >/dev/null
    for attempt in $(seq 1 60); do
        if docker exec "$postgres_name" \
            pg_isready -U turbodb -d turbodb >/dev/null 2>&1; then
            break
        fi
        if [ "$attempt" -eq 60 ]; then
            echo "PostgreSQL readiness timeout" >&2
            exit 1
        fi
        sleep 1
    done
    postgres_port="$(docker inspect -f \
        '{{(index (index .NetworkSettings.Ports "5432/tcp") 0).HostPort}}' \
        "$postgres_name")"
    postgres_conninfo="host=127.0.0.1 port=$postgres_port dbname=turbodb user=turbodb password=turbodb connect_timeout=5"
fi

docker run --name "$container_name" \
    --network host \
    --mount "type=bind,src=$run_root,dst=/work" \
    --mount "type=bind,src=/opt/vcpkg,dst=/opt/vcpkg" \
    --mount "type=bind,src=/var/cache/vcpkg,dst=/var/cache/vcpkg" \
    -e VCPKG_ROOT=/opt/vcpkg \
    -e VCPKG_DEFAULT_BINARY_CACHE=/var/cache/vcpkg \
    -e DEBIAN_FRONTEND=noninteractive \
    -e TURBODB_EU_POSTGRES_LIVE="$enable_postgres_live" \
    -e TURBODB_ORM_PGSQL_TEST_CONNINFO="$postgres_conninfo" \
    -e TURBODB_DBTOOLS_PG_TEST_CONNINFO="$postgres_conninfo" \
    ubuntu:24.04 bash -lc '
set -Eeuo pipefail

apt-get update
apt-get install -y --no-install-recommends \
    autoconf automake bison build-essential ca-certificates cmake curl flex \
    git libtool nasm ninja-build perl pkg-config python3 re2c unzip zip

for tool in autoreconf automake bison flex libtoolize nasm ninja perl python3 re2c; do
    command -v "$tool" >/dev/null
done

{
    date -u --iso-8601=seconds
    uname -a
    gcc --version
    cmake --version
    ninja --version
    autoreconf --version | head -n 1
    bison --version | head -n 1
    flex --version
    nasm --version
    re2c --version
    /opt/vcpkg/vcpkg version
} | tee /work/artifacts/environment.txt

cd /work/src/turbo-utils
cmake --fresh --preset linux-release-user \
    -DCMAKE_INSTALL_PREFIX=/opt/turboutils/release \
    -DTURBO_ENABLE_EPOLL_READINESS=ON \
    -DENABLE_TESTS=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_TESTING=OFF
cmake --build --preset linux-release-user
cmake --build --preset install-linux-release-user

cd /work/src/turbodb
orm_preset=linux-release-user
orm_install_preset=install-linux-release-user
if [ "$TURBODB_EU_POSTGRES_LIVE" = 1 ]; then
    orm_preset=linux-release-pg-live-user
    orm_install_preset=install-linux-release-pg-live-user
fi
cmake --fresh --preset "$orm_preset" \
    -DTIDESDB_BUILD_TESTS=OFF \
    -DORM_WITH_TIDESDB=OFF \
    -DORM_WITH_REDIS=ON \
    -DTURBODB_BUILD_DBTOOLS=ON \
    -DTURBODB_DBTOOLS_WITH_SQLITE=ON \
    -DENABLE_TESTS=ON \
    -DBUILD_TESTING=ON
cmake --build --preset "$orm_preset"
ctest --preset "$orm_preset" \
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

cmake --build --preset "$orm_install_preset"

/opt/turbodb/release/bin/turbodb-sqlite --help
if [ "$TURBODB_EU_POSTGRES_LIVE" = 1 ]; then
    /opt/turbodb/release/bin/turbodb-postgresql --help
fi
sha256sum /opt/turbodb/release/bin/turbodb-sqlite \
    > /work/artifacts/dbtools-binaries.sha256
if [ "$TURBODB_EU_POSTGRES_LIVE" = 1 ]; then
    sha256sum /opt/turbodb/release/bin/turbodb-postgresql \
        >> /work/artifacts/dbtools-binaries.sha256
fi

if [ "$TURBODB_EU_POSTGRES_LIVE" = 1 ]; then
    shared_consumer_build=/work/package-consumer-shared
    cmake --fresh \
        -S /work/src/turbodb/orm/tests/package_consumer/postgresql \
        -B "$shared_consumer_build" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="/opt/turbodb/release;/opt/turboutils/release;/work/src/turbodb/vcpkg_installed_pg/x64-linux"
    cmake --build "$shared_consumer_build"
    LD_LIBRARY_PATH="/opt/turbodb/release/lib:/opt/turboutils/release/lib:/work/src/turbodb/vcpkg_installed_pg/x64-linux/lib:${LD_LIBRARY_PATH:-}" \
        "$shared_consumer_build/orm_postgresql_c_consumer"
    LD_LIBRARY_PATH="/opt/turbodb/release/lib:/opt/turboutils/release/lib:/work/src/turbodb/vcpkg_installed_pg/x64-linux/lib:${LD_LIBRARY_PATH:-}" \
        "$shared_consumer_build/orm_postgresql_cpp_consumer"

    echo "Installed package consumers verified: shared x C/C++"
fi
' 2>&1 | tee "$run_root/artifacts/linux-build-test.log"

cleanup_container

sha256sum \
    "$run_root/artifacts/source.sha256" \
    "$run_root/artifacts/source.revisions.txt" \
    "$run_root/artifacts/container-image.txt" \
    "$run_root/artifacts/environment.txt" \
    "$run_root/artifacts/dbtools-binaries.sha256" \
    "$run_root/artifacts/linux-build-test.log" \
    "$run_root/artifacts/turbodb-linux-release.xml" \
    > "$run_root/artifacts/SHA256SUMS"

test "$(docker ps -aq --filter "name=^/${container_name}$" | wc -l)" -eq 0
test "$(docker ps -aq --filter "name=^/${postgres_name}$" | wc -l)" -eq 0

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
启用 live 模式时，configure 会验证 conninfo，且 `orm_postgres_live` 必须与
其余测试一起出现在同一个 JUnit；连接失败会使测试失败，不会 skip。

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
- `dbtools-binaries.sha256` 记录本次安装的 SQLite 及可选 PostgreSQL 工具摘要。
- `turbodb-linux-release.xml` 中测试数大于零，且 `failures`、`errors`、`skipped` 都为零。
- `SHA256SUMS` 能复验全部证据文件。

## 7. 成功判定

只有同时满足以下条件，才能声明本次 EU 远程测试通过：

1. 上传前与远端解压前的源码包 SHA-256 校验均通过。
2. TurboUtils 以 `TURBO_ENABLE_EPOLL_READINESS=ON` 成功构建并安装。
3. TurboDB 成功 configure/build，且独立 TidesDB engine tests 被关闭。
4. CTest 实际发现至少一个测试，JUnit 的 failure、error 与 skipped 数均为零。
5. Redis/CFlow 与 ORM contract tests 在同一次 run 内通过。
6. standalone SQLite package contract 通过；live 模式还必须通过 PostgreSQL driver/live test，
   并生成已安装工具的 SHA-256。
7. live 模式使用专用 PostgreSQL preset，并通过 shared × C/C++ 安装包 consumer matrix。
8. 精确命名的构建容器和可选 PostgreSQL 容器已被删除，run 目录和证据文件仍保留。

## 8. 常见失败

### `re2c` 不存在

TurboUtils 的生成步骤需要 `re2c`。确认 Ubuntu 容器安装命令包含 `re2c`，然后创建新的 run；不要复用已经部分配置的 build tree。

### BoringSSL 构建报告 `Could not find nasm`

PostgreSQL feature 会通过 libpq 引入 BoringSSL。确认 Ubuntu 容器安装命令
包含 `nasm`；这是构建环境缺失，不应通过关闭 TLS 依赖或跳过 PostgreSQL
测试规避。

### libpq 构建报告缺少 `bison`、`flex` 或 `perl`

仓库 overlay 的 `vcpkg-overlays/libpq/portfile.cmake` 明确要求这三个 host
工具；vcpkg 的 `vcpkg_configure_make()` 还会调用 Autotools。runbook 在
configure 前逐一执行 `command -v`；preflight 失败时先修正 builder 依赖集，
不要反复尝试不完整镜像。

### JUnit 校验报告 `No module named 'xml'`

证据校验使用 Python 标准库的 `xml.etree`。builder 必须安装完整的
`python3` 包，不能只安装 `python3-minimal`；后者在 Ubuntu 24.04 中不包含
该模块。不要跳过 JUnit 结构校验，因为 CTest 的控制台摘要不能替代持久化的
tests/failures/errors/skipped 证据。

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
