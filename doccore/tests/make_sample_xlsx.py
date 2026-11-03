"""Regenerates tests/data/sample.xlsx (the third-party fixture for P6).

The fixture exists so the reader is tested against a file produced by a
completely unrelated implementation, not by our own writer. It deliberately
mixes inline strings, shared strings, styles, merges, dates and formulas.

    pip install openpyxl
    python tests/make_sample_xlsx.py
"""
import datetime
import os

import openpyxl
from openpyxl.styles import Alignment, Font, PatternFill

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "sample.xlsx")


def main():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Report"
    ws["A1"], ws["B1"], ws["C1"], ws["D1"] = "Region", "Amount", "Share", "Date"
    rows = [("North", 1200.5), ("South", 800.25), ("East", 640.0)]
    for i, (name, amount) in enumerate(rows, start=2):
        ws.cell(row=i, column=1, value=name)
        ws.cell(row=i, column=2, value=amount)
        ws.cell(row=i, column=3, value="=B%d/B$6" % i)
        ws.cell(row=i, column=4, value=datetime.date(2024, 3, i))
    ws["A5"] = "Total"
    ws["B5"] = "=SUM(B2:B4)"
    ws["B6"] = 2640.75

    bold = Font(bold=True, name="Arial", size=12)
    fill = PatternFill("solid", fgColor="FFFF00")
    for cell in ws[1]:
        cell.font = bold
        cell.fill = fill
        cell.alignment = Alignment(horizontal="center")
    for r in range(2, 6):
        ws.cell(row=r, column=2).number_format = "#,##0.00"
        ws.cell(row=r, column=3).number_format = "0.0%"
        ws.cell(row=r, column=4).number_format = "yyyy-mm-dd"

    ws.merge_cells("A7:D7")
    ws["A7"] = "merged note"
    ws.column_dimensions["A"].width = 16
    ws.row_dimensions[1].height = 24
    ws.freeze_panes = "A2"
    wb.create_sheet("Empty")
    wb.save(OUT)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
