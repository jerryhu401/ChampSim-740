"""Generate convergence.pdf for the poster using matplotlib + seaborn."""
from __future__ import annotations

import matplotlib.pyplot as plt
import seaborn as sns

sns.set_theme(style="whitegrid", context="poster")

# (iter, score) per prefetcher --- best score so far at each iteration.
bop = [
    (0, 1.66), (1, 1.85), (2, 1.96), (3, 2.02), (4, 2.05),
    (5, 2.06), (6, 2.08), (7, 2.08), (8, 2.08), (10, 2.08),
    (12, 2.08), (15, 2.08), (20, 2.08),
]
ipcp = [
    (0, 1.64), (1, 2.18), (2, 2.18), (3, 2.18), (5, 2.18),
    (7, 2.18), (10, 2.18), (15, 2.18), (20, 2.18),
]
berti = [
    (0, 1.92), (1, 2.13), (3, 2.14), (5, 2.15), (7, 2.16),
    (9, 2.17), (12, 2.18), (15, 2.20), (17, 2.20), (19, 2.12),
    (20, 2.12),
]

CMU_RED = "#C41230"
ACCENT  = "#1E5AA0"
GOOD    = "#148240"

fig, ax = plt.subplots(figsize=(11, 4.0))

def plot_series(data, color, marker, label):
    xs = [p[0] for p in data]
    ys = [p[1] for p in data]
    ax.plot(xs, ys, marker=marker, markersize=11, linewidth=3.0,
            color=color, label=label,
            markeredgecolor="white", markeredgewidth=1.2)

plot_series(bop,   ACCENT,  "s", "BOP")
plot_series(ipcp,  GOOD,    "^", "IPCP")
plot_series(berti, CMU_RED, "o", "Berti")

# Annotate best points with arrow callouts placed clear of all curves.
ax.annotate(
    "BOP best (iter 6, 2.08)", xy=(6, 2.08), xytext=(8.2, 1.78),
    fontsize=15, color=ACCENT, fontweight="bold",
    arrowprops=dict(arrowstyle="->", color=ACCENT, lw=1.6),
)
ax.annotate(
    "IPCP best (iter 1, 2.18)", xy=(1, 2.18), xytext=(2.5, 2.30),
    fontsize=15, color=GOOD, fontweight="bold",
    arrowprops=dict(arrowstyle="->", color=GOOD, lw=1.6),
)
ax.annotate(
    "Berti best (iter 15, 2.20)", xy=(15, 2.20), xytext=(11.0, 2.32),
    fontsize=15, color=CMU_RED, fontweight="bold",
    arrowprops=dict(arrowstyle="->", color=CMU_RED, lw=1.6),
)

ax.set_xlim(-0.5, 20.5)
ax.set_ylim(1.55, 2.40)
ax.set_xlabel("Iteration", fontsize=18, fontweight="bold")
ax.set_ylabel("Best combined score", fontsize=18, fontweight="bold")
ax.tick_params(axis="both", labelsize=15)
ax.legend(loc="lower right", frameon=True, fontsize=15, ncol=3,
          bbox_to_anchor=(1.0, 0.0))

sns.despine(ax=ax)
# Reserve enough left margin so the y-axis label is never clipped.
fig.subplots_adjust(left=0.10, right=0.98, top=0.95, bottom=0.18)
fig.savefig("convergence.pdf")
print("wrote convergence.pdf")
