"""Regenerates tests/data/sample_dv.xlsx (third-party fixture for P9 data validation).

Produced with openpyxl so the data-validation reader is tested against a foreign
writer. Covers both list sources P9 must support:

  - an inline comma list  ("Yes,No,Maybe")      applied to B2:B6
  - a range reference      ($G$2:$G$4)          applied to D2:D6
    (the allowed values live in G2:G4)

Run:  python tests/make_sample_dv.py
"""
import os

import openpyxl
from openpyxl.worksheet.datavalidation import DataValidation

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "sample_dv.xlsx")


def main():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Choices"

    ws["A1"] = "Inline list"
    ws["B1"] = "Yes/No/Maybe"
    # B2:B6 命中内联列表 "Yes,No,Maybe"；初始值都在列表内。
    inline = ["Yes", "No", "Maybe", "Yes", "No"]
    for i, v in enumerate(inline, start=2):
        ws.cell(row=i, column=2, value=v)

    ws["C1"] = "Range list"
    ws["D1"] = "Low/Medium/High"
    # G2:G4 提供合法取值来源；D2:D6 引用该区域。
    for i, v in enumerate(["Low", "Medium", "High"], start=2):
        ws.cell(row=i, column=7, value=v)  # G 列
    rng = ["Low", "High", "Medium", "Low", "High"]
    for i, v in enumerate(rng, start=2):
        ws.cell(row=i, column=4, value=v)  # D 列

    dv_inline = DataValidation(type="list", formula1='"Yes,No,Maybe"', allow_blank=True)
    dv_inline.errorTitle = "Status"
    dv_inline.error = "Pick Yes, No or Maybe."
    dv_inline.showErrorMessage = True
    ws.add_data_validation(dv_inline)
    dv_inline.add("B2:B6")

    dv_range = DataValidation(type="list", formula1="$G$2:$G$4", allow_blank=True)
    dv_range.showErrorMessage = True
    ws.add_data_validation(dv_range)
    dv_range.add("D2:D6")

    wb.save(OUT)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
