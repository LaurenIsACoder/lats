#!/bin/bash

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

# =========================================
# lats vgpu 编译脚本
# 版本: 1.0
# 功能: 编译前检查依赖、设置环境变量、修复.pc文件
# =========================================

BUILDMODE="vgpu"

help() {
    echo "Usage:"
    echo "  -c              [configure]"
    echo "                  release   : default value"
    echo "                  dbg       : debug"
    echo "                  dbg-opt   : debug opt"
    echo "  -h              help"
}

parseArgs() {
    while getopts "c:h" opt; do
        case ${opt} in
        c)
            if [ "$OPTARG" == "release" ] ; then
                BUILDMODE="vgpu"
            elif [ "$OPTARG" == "dbg" ] ; then
                BUILDMODE="vgpu-dbg"
            elif [ "$OPTARG" == "dbg-opt" ] ; then
                BUILDMODE="vgpu-dbg-opt"
            else
                help
                exit
            fi
            ;;
        h)
            help
            exit
            ;;
        \?)
            help
            exit
            ;;
        esac
    done
}

# 设置错误处理
set -e  # 遇到错误立即退出

# 颜色输出定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 日志函数
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# =========================================
# 1. 检查并安装依赖包
# =========================================
install_dependencies() {
    log_info "检查并安装依赖包..."

    # 编译依赖
    local dependencies=(
        "libgbm-dev"
    )

    for dep in "${dependencies[@]}"; do
        if ! dpkg -l | grep -q "^ii  $dep"; then
            log_info "安装$dep..."
            sudo apt-get install -y "$dep"
        else
            log_info "$dep已安装"
        fi
    done

    log_info "所有依赖包检查完成"
}

# =========================================
# 2. 设置环境变量
# =========================================
setup_environment() {
    log_info "设置环境变量..."

    # 获取当前脚本的绝对路径
    SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

    # 设置 VGPU_RELEASE_PATH 为 lats 项目根目录下的 vgpu-release
    PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
    VGPU_RELEASE_PATH="$PROJECT_ROOT/vgpu-release"

    # 检查vgpu-release目录是否存在
    if [ ! -d "$VGPU_RELEASE_PATH" ]; then
        log_error "vgpu-release目录不存在: $VGPU_RELEASE_PATH"
        log_info "请确保已在lats项目根目录创建vgpu-release目录"
        log_info "当前目录结构:"
        find "$PROJECT_ROOT" -maxdepth 2 -type d | sort
        exit 1
    fi

    # 设置环境变量
    export VGPU_RELEASE_PATH
    export PKG_CONFIG_PATH="$VGPU_RELEASE_PATH/lib/pkgconfig:$VGPU_RELEASE_PATH/lib/loongarch64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH"
    export LD_LIBRARY_PATH="$VGPU_RELEASE_PATH/lib:$LD_LIBRARY_PATH"

    log_info "环境变量设置完成:"
    log_info "  PROJECT_ROOT: $PROJECT_ROOT"
    log_info "  VGPU_RELEASE_PATH: $VGPU_RELEASE_PATH"
    log_info "  PKG_CONFIG_PATH: $PKG_CONFIG_PATH"
    log_info "  LD_LIBRARY_PATH: $LD_LIBRARY_PATH"
}

# =========================================
# 3. 修复.pc文件中的prefix路径
# =========================================
fix_pkgconfig_files() {
    log_info "检查并修复.pc文件..."

    local pc_dirs=(
        "$VGPU_RELEASE_PATH/lib/pkgconfig"
        "$VGPU_RELEASE_PATH/lib/loongarch64-linux-gnu/pkgconfig"
    )

    local fixed_count=0
    local checked_count=0

    for pc_dir in "${pc_dirs[@]}"; do
        if [ ! -d "$pc_dir" ]; then
            log_warn "pkgconfig目录不存在: $pc_dir"
            continue
        fi

        log_info "检查目录: $pc_dir"

        # 查找所有.pc文件
        find "$pc_dir" -name "*.pc" -type f | while read pc_file; do
            checked_count=$((checked_count + 1))

            # 检查当前prefix
            current_prefix=$(grep -E "^prefix=" "$pc_file" | head -1 | cut -d= -f2-)

            if [ -n "$current_prefix" ] && [ "$current_prefix" != "$VGPU_RELEASE_PATH" ]; then
                log_info "修复: $(basename "$pc_file")"
                log_info "  原prefix: $current_prefix"
                log_info "  新prefix: $VGPU_RELEASE_PATH"

                # 备份原文件
                cp "$pc_file" "${pc_file}.bak"

                # 使用sed替换prefix
                sed -i "s|^prefix=.*$|prefix=$VGPU_RELEASE_PATH|" "$pc_file"

                # 同时替换可能存在的${prefix}引用
                #sed -i "s|\${prefix}|$VGPU_RELEASE_PATH|g" "$pc_file"

                fixed_count=$((fixed_count + 1))

                # 验证修复结果
                new_prefix=$(grep -E "^prefix=" "$pc_file" | head -1 | cut -d= -f2-)
                if [ "$new_prefix" = "$VGPU_RELEASE_PATH" ]; then
                    log_info "  ✓ 修复成功"
                else
                    log_warn "  ✗ 修复失败，恢复备份"
                    mv "${pc_file}.bak" "$pc_file"
                fi
            fi
        done
    done

    log_info "PC文件检查完成:"
    log_info "  检查文件数: $checked_count"
    log_info "  修复文件数: $fixed_count"

    if [ $fixed_count -eq 0 ] && [ $checked_count -gt 0 ]; then
        log_info "所有.pc文件的prefix都已正确设置为VGPU_RELEASE_PATH"
    fi
}

# =========================================
# 4. 验证环境设置
# =========================================
verify_environment() {
    log_info "验证环境设置..."

    # 检查关键目录
    local required_dirs=(
        "$VGPU_RELEASE_PATH"
        "$VGPU_RELEASE_PATH/lib"
    )

    for dir in "${required_dirs[@]}"; do
        if [ -d "$dir" ]; then
            log_info "✓ 目录存在: $dir"
        else
            log_error "目录不存在: $dir"
            return 1
        fi
    done

    # 检查.pc文件是否存在
    local pc_files=$(find "$VGPU_RELEASE_PATH/lib/pkgconfig" "$VGPU_RELEASE_PATH/lib/loongarch64-linux-gnu/pkgconfig" -name "*.pc" 2>/dev/null | wc -l)
    if [ "$pc_files" -gt 0 ]; then
        log_info "✓ 找到$pc_files个.pc文件"
    else
        log_warn "未找到.pc文件，可能需要先编译安装"
    fi

    # 验证pkg-config是否能找到库
    if command -v pkg-config >/dev/null 2>&1; then
        log_info "测试pkg-config..."
        pkg-config --list-all | head -5
    fi

    log_info "环境验证完成"
}

# =========================================
# 5. 编译准备
# =========================================
prepare_build() {
    log_info "准备编译..."

    # 创建构建目录（如果在脚本所在目录）
    BUILD_DIR="$SCRIPT_DIR/build_output"
    if [ ! -d "$BUILD_DIR" ]; then
        mkdir -p "$BUILD_DIR"
        log_info "创建构建目录: $BUILD_DIR"
    fi

    # 显示可用的编译选项
    log_info "可用编译选项示例:"
    echo "  ./configure --prefix=$VGPU_RELEASE_PATH"
    echo "  或"
    echo "  meson setup builddir --prefix=$VGPU_RELEASE_PATH"

    # 提示用户
    log_info "请根据您的项目选择适当的编译命令"
    log_info "环境变量已设置完成，可以开始编译"
}

# =========================================
# 7. 主函数
# =========================================
main() {
    echo -e "${GREEN}"
    echo "========================================"
    echo "        lats vgpu 编译"
    echo "========================================"
    echo -e "${NC}"

    # 执行各个步骤
    install_dependencies
    setup_environment
    fix_pkgconfig_files
    verify_environment
    ./latxbuild/build.sh -c ${BUILDMODE}
}

parseArgs "$@"
main
