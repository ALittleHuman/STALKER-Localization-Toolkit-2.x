# -*- coding: utf-8 -*-
"""坏夹具 ②：**语法错**（插件改装 P2b）。

`exec_module` 阶段就炸 ⇒ 连 `register` 都没有机会跑。用途：证明"导入失败"这一类
与"注册失败"那一类**都**会：逐条留痕 + 不阻塞启动 + 不拖累同目录的好插件。
"""


def register(api)      # ← 故意缺冒号：SyntaxError
    pass
