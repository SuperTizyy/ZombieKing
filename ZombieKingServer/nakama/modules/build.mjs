// esbuild 打包：TS -> 单文件 JS。
// 必须用 format=esm 且入口不导出任何符号，使 InitModule 保持在 AST 顶层
// （Nakama JS 运行时按顶层 AST 静态提取钩子函数，见 main.ts 注释）。
import { build } from "esbuild";

await build({
  entryPoints: ["src/main.ts"],
  bundle: true,
  format: "esm",
  treeShaking: false,
  target: "es2017",
  platform: "neutral",
  outfile: "dist/index.js",
  legalComments: "none",
});

console.log("built dist/index.js");
