/* ==========================================================================
 * MyOsHello.java  -  myOS に Java が載っていることを確かめるための小さな窓
 *
 * Swing (AWT) が動く = GUI 用の Java が揃っている、ということの確認になる。
 * Minecraft のような LWJGL アプリはこれに加えて OpenGL を要求するので、
 * デスクトップの "OpenGL Test" (glxgears) と合わせて見ること。
 *
 * Java のバイトコードは可搬なので、ホスト側でコンパイルしたものを
 * そのまま rootfs に置いている。rootfs 側に JDK は要らない (JRE だけでよい)。
 * ========================================================================== */
import javax.swing.*;
import java.awt.*;

public class MyOsHello {

    /* Windows 98 の配色 */
    private static final Color FACE     = new Color(0xC0, 0xC0, 0xC0);
    private static final Color LIGHT    = Color.WHITE;
    private static final Color SHADOW   = new Color(0x80, 0x80, 0x80);
    private static final Color DKSHADOW = Color.BLACK;

    /* 2 重の立体枠。myOS の GUI 全体で使っている作法に合わせる。 */
    private static void bevel(Graphics g, int x, int y, int w, int h,
                              boolean raised) {
        g.setColor(raised ? LIGHT : SHADOW);
        g.drawLine(x, y, x + w - 1, y);
        g.drawLine(x, y, x, y + h - 1);
        g.setColor(raised ? DKSHADOW : LIGHT);
        g.drawLine(x, y + h - 1, x + w - 1, y + h - 1);
        g.drawLine(x + w - 1, y, x + w - 1, y + h - 1);
        g.setColor(raised ? FACE : DKSHADOW);
        g.drawLine(x + 1, y + 1, x + w - 2, y + 1);
        g.drawLine(x + 1, y + 1, x + 1, y + h - 2);
        g.setColor(raised ? SHADOW : FACE);
        g.drawLine(x + 1, y + h - 2, x + w - 2, y + h - 2);
        g.drawLine(x + w - 2, y + 1, x + w - 2, y + h - 2);
    }

    public static void main(String[] args) {
        SwingUtilities.invokeLater(() -> {
            JFrame f = new JFrame("Java on myOS");

            JPanel p = new JPanel() {
                @Override
                protected void paintComponent(Graphics g) {
                    super.paintComponent(g);
                    int w = getWidth(), h = getHeight();
                    g.setColor(FACE);
                    g.fillRect(0, 0, w, h);
                    bevel(g, 8, 8, w - 16, h - 16, false);

                    g.setColor(Color.BLACK);
                    g.setFont(new Font(Font.SANS_SERIF, Font.BOLD, 16));
                    g.drawString("Java is running on myOS", 28, 46);

                    g.setFont(new Font(Font.MONOSPACED, Font.PLAIN, 12));
                    String[] lines = {
                        "java.version  : " + System.getProperty("java.version"),
                        "java.vendor   : " + System.getProperty("java.vendor"),
                        "java.home     : " + System.getProperty("java.home"),
                        "os.name       : " + System.getProperty("os.name"),
                        "os.arch       : " + System.getProperty("os.arch"),
                        "",
                        "Booted by a hand-written bootloader:",
                        "  stage1 (MBR, 512 bytes)",
                        "    -> stage2_linux (Linux 32-bit boot protocol)",
                        "      -> Linux kernel",
                        "        -> Xorg + myos-wm",
                        "          -> this JVM",
                    };
                    int y = 76;
                    for (String s : lines) {
                        g.drawString(s, 28, y);
                        y += 18;
                    }
                }
            };
            p.setPreferredSize(new Dimension(560, 360));

            f.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
            f.add(p);
            f.pack();
            f.setVisible(true);
        });
    }
}
