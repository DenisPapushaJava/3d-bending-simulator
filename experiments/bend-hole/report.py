#!/usr/bin/env python3
"""Create portable postprocessing artifacts from completed CalculiX results. MIT."""

import argparse
import html
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

from bend_hole import boundary
from results import analyze, tool_outline


def viewer(directory, target, parsed, provenance=None):
    result, model, nodes, elements, groups, frames = parsed
    faces = [face for _, _, face in boundary(elements, groups["PLATE"])
             if not (model.half_model and all(abs(nodes[n][2]) < 1e-8 for n in face))]
    ids = sorted({n for face in faces for n in face})
    mapping = {n: i for i, n in enumerate(ids)}
    data = {"name": directory.name, "model": result["input"]["model"],
            "nodes": [nodes[n] for n in ids], "faces": [[mapping[n] for n in face] for face in faces],
            "tools": {name.lower(): tool_outline(model, name, 12) for name in ("PUNCH", "DIE")},
            "frames": [{"time": 0, "u": [0]*(len(ids)*3), "pe": [0]*len(ids),
                        "stroke": 0, "force": 0, "maxPe": 0}]}
    for entry, (time, frame) in zip(result["history"], sorted(frames.items())):
        data["frames"].append({"time": time, "stroke": entry["stroke_mm"], "force": entry["punch_force_N"],
                               "maxPe": entry["max_nodal_averaged_peeq"],
                               "u": [round(value, 7) for n in ids for value in frame["DISP"][n]],
                               "pe": [round(frame.get("PE", {}).get(n, (0,))[0], 7) for n in ids]})
    if provenance is not None:
        data["provenance"] = provenance
    data["formedIndex"] = min(range(1, len(data["frames"])), key=lambda i: abs(data["frames"][i]["time"]-1))
    template = Path(__file__).with_name("viewer.html").read_text(encoding="utf-8")
    payload = json.dumps(data, separators=(",", ":"), allow_nan=False).replace("</", "<\\/")
    target.write_text(template.replace("__DATA__", payload), encoding="utf-8")


def plot_shape(ax, parsed, title):
    result, model, nodes, elements, groups, frames = parsed
    frame = frames[min(frames, key=lambda value: abs(value-1))]
    polygons, colors = [], []
    for _, _, face in boundary(elements, groups["PLATE"]):
        if model.half_model and all(abs(nodes[n][2]) < 1e-8 for n in face):
            continue
        count = len(face)//2
        order = [face[i] for j in range(count) for i in (j, j+count)]
        for mirror in ([1, -1] if model.half_model else [1]):
            points = np.array([np.array(nodes[n])+frame["DISP"][n] for n in order])
            points[:, 2] *= mirror
            polygons.append(points[:, [0, 2, 1]])
            colors.append(np.mean([frame.get("PE", {}).get(n, (0,))[0] for n in order]))
    collection = Poly3DCollection(polygons, linewidths=0, cmap="viridis", clim=(0, .15))
    collection.set_array(np.array(colors))
    ax.add_collection3d(collection)
    ax.set(xlim=(-32, 32), ylim=(-model.width/2-1, model.width/2+1), zlim=(-4, 6),
           xlabel="X, мм", ylabel="Z, мм", zlabel="Y, мм", title=title)
    ax.set_box_aspect((64, model.width+2, 16))
    ax.view_init(elev=40, azim=-65)
    return collection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("study", type=Path)
    args = parser.parse_args()
    output = args.study / "report"
    output.mkdir(exist_ok=True)
    parsed = {}
    for directory in sorted(args.study.iterdir()):
        if (directory / "result.json").exists():
            parsed[directory.name] = analyze(directory)
    if not {"beam", "control", "center-medium", "center-coarse", "center-fine", "offset"} <= set(parsed):
        raise SystemExit("The main study cases are incomplete")
    for name, value in parsed.items():
        if name != "beam":
            viewer(args.study/name, output/f"{name}.html", value)
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10})
    fig = plt.figure(figsize=(15, 10), layout="constrained")
    ax1 = fig.add_subplot(2, 2, 1, projection="3d")
    ax2 = fig.add_subplot(2, 2, 2, projection="3d")
    collection = plot_shape(ax1, parsed["center-medium"], "Отверстие на линии гиба · ход 3 мм")
    plot_shape(ax2, parsed["offset"], "Отверстие смещено на 6 мм · ход 3 мм")
    fig.colorbar(collection, ax=[ax1, ax2], shrink=.6, extend="max", label="PEEQ · усреднение в узлах (0,150+)")
    ax3 = fig.add_subplot(2, 2, 3)
    for name, label in [("control", "Без отверстия"), ("center-medium", "Отверстие X=0"), ("offset", "Отверстие X=6")]:
        history = parsed[name][0]["history"]
        ax3.plot([v["stroke_mm"] for v in history], [v["punch_force_N"] for v in history], label=label)
    ax3.set(xlabel="Ход пуансона, мм", ylabel="Сила на всю ширину, Н", title="Контакт без трения · условный изотропный материал")
    ax3.grid(alpha=.25)
    ax3.set_ylim(bottom=0)
    ax3.legend()
    ax4 = fig.add_subplot(2, 2, 4)
    for name, label in [("center-coarse", "Крупная сетка"), ("center-medium", "Средняя сетка"), ("center-fine", "Мелкая сетка")]:
        result = parsed[name][0]
        ax4.plot([v["stroke_mm"] for v in result["history"]], [v["punch_force_N"] for v in result["history"]], label=f"{label}: {result['input']['plate_elements']} элементов")
    ax4.set(xlabel="Ход пуансона, мм", ylabel="Сила на всю ширину, Н", title="Чувствительность к сетке · отверстие X=0")
    ax4.grid(alpha=.25)
    ax4.set_ylim(bottom=0)
    ax4.legend()
    fig.suptitle("FEM-эксперимент: пластина 60 × 20 × 1 мм, отверстие Ø4 мм, V24, носик R2\nИсследовательский результат · точность по реальным образцам не установлена · разрушение не рассчитывается", fontsize=13)
    fig.savefig(output/"comparison.png", dpi=150)
    plt.close(fig)
    summary = {}
    for name, (result, *_) in parsed.items():
        summary[name] = {key: value for key, value in result.items() if key != "input"}
        summary[name]["input"] = {key: value for key, value in result["input"].items()
                                  if key not in {"plate_nodes", "plate_boundary"}}
        summary[name]["mesh_check"] = json.loads((args.study/name/"mesh-check.json").read_text(encoding="utf-8"))
        summary[name]["run"] = json.loads((args.study/name/"run.json").read_text(encoding="utf-8"))
    (output/"summary.json").write_text(json.dumps(summary, indent=2, allow_nan=False)+"\n", encoding="utf-8")
    labels = {"control": "Без отверстия", "center-coarse": "По центру · крупная сетка",
              "center-medium": "По центру · средняя сетка", "center-fine": "По центру · мелкая сетка",
              "offset": "Отверстие X=6 мм", "small-step": "Уменьшенный шаг нагрузки",
              "stiff-contact": "Удвоенная жёсткость контакта", "unloading": "Гибка и разгрузка"}
    rows = []
    for name, result in summary.items():
        if name == "beam":
            continue
        formed = min(result["history"], key=lambda item: abs(item["time"]-1))
        final_angle = f"{result['final_flange_angle_deg']:.2f}°" if result["input"]["model"]["unload"] else "—"
        rows.append(f'<tr><td><a href="{name}.html">{html.escape(labels.get(name, name))}</a></td>'
                    f'<td>{formed["punch_force_N"]:.2f} Н</td><td>{result["formed_flange_angle_deg"]:.2f}°</td>'
                    f'<td>{final_angle}</td></tr>')
    index = '''<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Гибка с отверстием — результаты FEM</title><style>body{font:16px system-ui,sans-serif;color:#19314b;background:#f1f5f9;margin:30px auto;max-width:1100px;padding:0 24px}h1{font-size:28px}p{line-height:1.6}table{border-collapse:collapse;width:100%;background:white}th,td{text-align:left;padding:14px;border-bottom:1px solid #dae2ec}a{color:#1465a2}img{max-width:100%;margin:25px 0}.note{padding:18px;background:#fff5df;border:1px solid #efcf88;border-radius:8px}</style>
<h1>Пластина с отверстием: первый FEM-эксперимент</h1><p>60 × 20 × 1 мм · отверстие Ø4 мм · матрица V24 · пуансон R2 · ход 3 мм.</p>
<p class="note">Исследовательская модель: условный изотропный материал, без трения и повреждения. Точность по реальным образцам пока не установлена. Численные результаты не являются подтверждением изготовления.</p>
<p>Откройте анимацию нужного случая: ползунок, воспроизведение/пауза, вращение и масштаб. Перемещения показаны ×1. Угол измерен по наилучшим плоскостям дальних полок; плоскость соответствует 0°.</p>
<table><thead><tr><th>Анимация</th><th>Сила в конце гибки</th><th>Угол под нагрузкой</th><th>После разгрузки</th></tr></thead><tbody>'''
    index += "".join(rows) + '''</tbody></table><img src="comparison.png" alt="Форма пластины, поле PEEQ, сила и чувствительность к сетке">
<p>Отдельная упругая проверка шириной 4 мм: 1,9449 Н при аналитических 1,9894 Н (отличие 2,24%). Это проверка простой балки, а не точность прогноза отверстия.</p>
<p><a href="summary.json">Числа, параметры и происхождение результатов (JSON)</a>. Цвет PEEQ — накопленная пластическая деформация с усреднением в узлах; он не определяет вероятность трещины.</p></html>'''
    (output/"index.html").write_text(index, encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
