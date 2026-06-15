# OnlineJudge 开发环境安装指南
---
## 步骤 1：更新 apt 源
```bash
sudo apt update
```

## 步骤 2：安装构建工具链

```bash
sudo apt install -y cmake ninja-build pkg-config
```

## 步骤 3：安装基础开发工具

```bash
sudo apt install -y g++ git curl jq make
```

## 步骤 4：安装 3 个 C 系统库（dev 版，含头文件）

```bash
sudo apt install -y default-libmysqlclient-dev libargon2-dev libcurl4-openssl-dev
```

## 步骤 5：安装 MySQL 客户端 + libargon2 运行期库

```bash
sudo apt install -y default-mysql-client libargon2-1
```

## 步骤 6：验证全部安装

```bash
echo "==== 验证输出 ===="
g++ --version | head -1
git --version
curl --version | head -1
jq --version
make --version | head -1
cmake --version | head -1
ninja --version
pkg-config --version
pkg-config --modversion mysqlclient libcurl libargon2
mysql --version
```
---