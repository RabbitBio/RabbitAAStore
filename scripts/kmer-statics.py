import argparse
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import sys
import os

# ================= 配置区域 =================
#INPUT_FILE = 'kmer_counts.txt' 
#OUTPUT_IMAGE = 'kmer_trend_overall.png'

# 目标采样点数 (控制绘图密度，2000个点足够画出平滑曲线)
TARGET_POINTS = 2000 
# ===========================================
def parse_args():
    """解析命令行参数"""
    parser = argparse.ArgumentParser(
        description="绘制 K-mer 频次分布趋势图 (Rank-Frequency Plot)",
        epilog="示例: python plot_kmer_cli.py -i kmer_counts.txt -o result.png"
    )
    
    # -i 输入文件 (必须)
    parser.add_argument('-i', '--input', required=True, help="输入文件路径 (包含 kmer 和 count 两列)")
    
    # -o 输出文件 (可选，默认 kmer_trend.png)
    parser.add_argument('-o', '--output', default='kmer_trend.png', help="输出图片路径 (默认: kmer_trend.png)")
    
    # -t 标题 (可选)
    parser.add_argument('-t', '--title', default=None, help="图表标题")

    return parser.parse_args()

def main():
    args = parse_args()

    input_file = args.input
    output_file = args.output

    # 1. 检查文件
    if not os.path.exists(input_file):
        print(f"Error: 输入文件 '{input_file}' 不存在。")
        sys.exit(1)

    print(f"正在读取文件: {input_file} ...")

#    if not os.path.exists(INPUT_FILE):
#        print(f"错误: 找不到文件 {INPUT_FILE}")
#        return
#
#    print(f"正在读取文件 {INPUT_FILE} ...")
    
    try:
        # 1. 读取数据 (自动忽略 # 开头的行)
        df = pd.read_csv(input_file, sep='\s+', comment='#', header=None, names=['kmer', 'count'])
        
        # 转换数值并排序
        df['count'] = pd.to_numeric(df['count'], errors='coerce')
        df.dropna(inplace=True)
        df.sort_values(by='count', ascending=False, inplace=True)
        
        # 重置索引，现在的 index 就是排名 (Rank)
        df.reset_index(drop=True, inplace=True)
        
        total_rows = len(df)
        print(f"原始数据共 {total_rows} 条。")

    except Exception as e:
        print(f"读取出错: {e}")
        return

    # 2. 智能抽样策略
    # 策略：头部数据变化剧烈，全部保留；尾部数据变化平缓，进行抽样。
    
    # 定义“头部”的大小 (例如前 50 个点不抽样)
    head_size = 50
    
    if total_rows > TARGET_POINTS:
        # 计算尾部的步长
        tail_step = (total_rows - head_size) // (TARGET_POINTS - head_size)
        tail_step = max(1, tail_step) # 保证步长至少为1
        
        # 头部：完整切片
        df_head = df.iloc[:head_size]
        
        # 尾部：抽样切片
        df_tail = df.iloc[head_size::tail_step]
        
        # 合并
        plot_data = pd.concat([df_head, df_tail])
        print(f"为了绘图性能，已从 {total_rows} 条数据中抽样出 {len(plot_data)} 个代表性数据点。")
    else:
        plot_data = df

    # 3. 绘图
    plt.figure(figsize=(12, 7))

    # X轴: 排名 (Rank), Y轴: 频次 (Count)
    # 使用 plot 画线，而不是 bar 画柱
    plt.plot(plot_data.index, plot_data['count'], color='#1f77b4', linewidth=1.5, alpha=0.8, label='Frequency Trend')
    
    # 填充颜色，让面积感更强
    plt.fill_between(plot_data.index, plot_data['count'], color='#1f77b4', alpha=0.1)

    # 4. 标注 Top Outliers (在图上标出前几个具体的 K-mer)
    top_n_annotate = 5
    for i in range(min(top_n_annotate, len(df))):
        row = df.iloc[i]
        kmer_seq = row['kmer']
        count_val = row['count']
        rank = i
        
        # 在点旁边写上 K-mer 名字
        plt.annotate(f"{kmer_seq}\n({int(count_val):,})", 
                     xy=(rank, count_val), 
                     xytext=(rank + total_rows*0.05, count_val), # 文字稍微偏移一点
                     arrowprops=dict(arrowstyle="->", connectionstyle="arc3,rad=.2", color='red'),
                     fontsize=9, color='darkred')
        
        # 画个红点强调一下
        plt.scatter([rank], [count_val], color='red', s=30, zorder=5)

    # 5. 设置坐标轴
    plt.yscale('log') # 必须用对数坐标
    plt.xlabel('Rank (Sorted by Frequency)', fontsize=12)
    plt.ylabel('Frequency (Log Scale)', fontsize=12)
    plt.title(f'K-mer Frequency Distribution (Overall Trend)\nTotal K-mers: {total_rows:,}', fontsize=14)
    
    # 添加网格
    plt.grid(True, which="both", ls="--", alpha=0.4)
    
    plt.tight_layout()
    plt.savefig(output_file, dpi=300)
    print(f"趋势图已保存: {output_file}")

if __name__ == "__main__":
    main()
