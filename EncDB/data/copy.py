import os
import shutil

# ====== 配置区 ======
source_filename = "/home/yotsugi/MyEncDB/EncDB/data/enron/7"  # 要复制的文件名
target_dir = "/home/yotsugi/MyEncDB/EncDB/data/test"        # 目标目录
copy_count = 50              # 复制次数
# ===================

def main():

    if not os.path.exists(source_filename):
        raise FileNotFoundError(f"源文件不存在: {source_filename}")

    # 创建目标目录（若不存在）
    os.makedirs(target_dir, exist_ok=True)

    # 获取文件扩展名

    for i in range(1, copy_count + 1):
        target_filename = f"{i}"
        target_path = os.path.join(target_dir, target_filename)

        shutil.copy2(source_filename, target_path)

    print(f"成功复制 {copy_count} 个文件到目录: {target_dir}")

if __name__ == "__main__":
    main()
