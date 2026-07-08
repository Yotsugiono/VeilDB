from collections import Counter
import os
import random, csv

def generate_normal_distribution_list(mu, sigma, n, max_val, min_val):
    random_list = [max(min_val, min(round(random.gauss(mu, sigma)), max_val)) for _ in range(n)]
    return random_list

# 示例用法
n = 10000  # 列表长度
bits = 32
mu = (2 ** bits) / 2 # 均值
sigma = mu/3.5  # 标准差

streaming_dir="32bits"


random_list = generate_normal_distribution_list(mu, sigma, n, (2 ** bits)-1, 0)
# print(random_list)

for i in range(0,n):
    with open(os.path.join(streaming_dir, str(i+1)), "w") as myfile:
        myfile.write(str(random_list[i]))
    myfile.close()


# 统计每个数字的频次
frequency_counter = Counter(random_list)

# 按数字从小到大排序
sorted_frequencies = sorted(frequency_counter.items(), key=lambda x: x[0])

# 写入CSV文件
with open('freq-'+streaming_dir+'-int.csv', 'w', newline='') as csvfile:
    writer = csv.writer(csvfile)
    for num, freq in sorted_frequencies:
        writer.writerow([num, freq])

