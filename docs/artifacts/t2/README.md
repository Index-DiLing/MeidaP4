# T2 真机截图（`docs/artifacts/t2/`）

这些都是**主机经 USB 视频通道（IF1 / EP3）收到的原始 JPEG**，不是屏幕截图、也不是重编码图：

| 文件 | 说明 |
|---|---|
| `frame-1280x720-q67.jpg` | 自适应降档期（quality≈67）：`FPS 15.84 (t25) ENC 8.9 ms RENDER 7.7 ms` |
| `frame-1280x720-q80.jpg` | 稳定期（quality=80 / target 30）：`FPS 22.77 (t30) ENC 8.9 ms RENDER 7.7 ms LINK 11314 kbps` |
| `frame-1280x720-safe-capacity.jpg` | 修复 `statvfs` 之后：`SD /sdcard 7.2 / 7.2 GiB free` + `QUALITY 80 / TARGET 30 fps` |

判读要点（肉眼即可验收）：
- 标题 `MeidaP4`、`Uptime`、`SD 容量`、`FPS/ENC/RENDER/LINK`、`FRAMES/BYTES/QUALITY/TARGET`、`SEQ` 全部清晰；
- 16 段彩虹色条颜色正确（证明 `pixel_reverse=false`，无红蓝互换）；
- 移动方块位置、进度条长度、`SEQ` 数值逐帧变化（证明不是静态图 / 画面未冻结）。

复现：
```powershell
python tools/webusb_app.py --video-seconds 10 --save-frames   # -> out/frame_*.jpg
```
