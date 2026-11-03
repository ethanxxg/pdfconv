"""Regenerates tests/data/sample_chart.xlsx (third-party fixture for P9 charts).

Produced with openpyxl so the chart reader is tested against a foreign writer.
The workbook deliberately covers the shapes P9 must render:

  Sheet "Report"   a clustered column chart (2 series) + a line chart + a pie chart
                   anchored on the sheet, plus a table with a conditional format
                   (cellIs greater-than -> red fill) and a list data validation.

Run:  python tests/make_sample_chart.py
"""
import os

import openpyxl
from openpyxl.chart import BarChart, LineChart, PieChart, Reference
from openpyxl.chart.label import DataLabelList
from openpyxl.formatting.rule import CellIsRule
from openpyxl.styles import Font, PatternFill
from openpyxl.worksheet.datavalidation import DataValidation

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "sample_chart.xlsx")


def main():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Report"

    ws["A1"] = "Region"
    ws["B1"] = "Q1"
    ws["C1"] = "Q2"
    rows = [("North", 10, 20), ("South", 40, 50), ("East", 30, 15), ("West", 25, 35)]
    for i, (name, q1, q2) in enumerate(rows, start=2):
        ws.cell(row=i, column=1, value=name)
        ws.cell(row=i, column=2, value=q1)
        ws.cell(row=i, column=3, value=q2)
    ws["A7"] = "Total"
    ws["B7"] = "=SUM(B2:B5)"
    ws["C7"] = "=SUM(C2:C5)"

    for cell in ws[1]:
        cell.font = Font(bold=True, name="Calibri", size=11)

    # 条件格式：Q1 大于 35 的格子填红。
    ws.conditional_formatting.add(
        "B2:B5",
        CellIsRule(operator="greaterThan", formula=["35"],
                   fill=PatternFill("solid", fgColor="FFFF0000")),
    )
    # 数据验证：Region 列只能是列出来的四个值。
    dv = DataValidation(type="list", formula1='"North,South,East,West"', allow_blank=True)
    dv.error = "Pick a region from the list."
    dv.errorTitle = "Region"
    dv.showErrorMessage = True
    ws.add_data_validation(dv)
    dv.add("A2:A5")

    # 簇状柱形图：两个系列，数据与分类都取自表内区域。
    bar = BarChart()
    bar.type = "col"
    bar.grouping = "clustered"
    bar.title = "Quarterly revenue"
    bar.y_axis.title = "Revenue"
    bar.x_axis.title = "Region"
    bar.height = 8.5
    bar.width = 16
    data = Reference(ws, min_col=2, max_col=3, min_row=1, max_row=5)
    cats = Reference(ws, min_col=1, min_row=2, max_row=5)
    bar.add_data(data, titles_from_data=True)
    bar.set_categories(cats)
    bar.dataLabels = DataLabelList()
    bar.dataLabels.showVal = True
    ws.add_chart(bar, "E2")

    # 折线图：同一个区域，只取 Q1。
    line = LineChart()
    line.title = "Q1 trend"
    line.height = 7.5
    line.width = 16
    ldata = Reference(ws, min_col=2, min_row=1, max_row=5)
    line.add_data(ldata, titles_from_data=True)
    line.set_categories(cats)
    ws.add_chart(line, "E20")

    # 饼图：Q2 占比。
    pie = PieChart()
    pie.title = "Q2 share"
    pie.height = 7.5
    pie.width = 12
    pdata = Reference(ws, min_col=3, min_row=1, max_row=5)
    pie.add_data(pdata, titles_from_data=True)
    pie.set_categories(cats)
    ws.add_chart(pie, "N2")

    wb.save(OUT)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
