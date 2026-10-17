package com.psp2i.recomp;

import android.Manifest;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.Settings;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.CountDownLatch;

import org.libsdl.app.SDLActivity;

/* SDL's activity runs main() from libmain.so (port/host/sdl_main.c), which
 * calls the two static helpers below from the game's thread:
 *
 *   openGameImage  the game's ISO, picked with Android's file picker once and
 *                  remembered (a persistable read grant: no storage permission)
 *   copyFonts      the PSP fonts, copied once from the flash/ folder of a
 *                  GameData folder the user picks, into the data folder
 *   dataFolder     where psp2i.ini, GameData (saves, fonts) and the log live:
 *                  PSP2i at the top of the shared storage, so the player can
 *                  open them with any file manager or over USB (not the
 *                  app-private Android/data folder, which Android 11+ hides)
 *
 * Both show their dialog on the UI thread and wait for the answer. */
public class PSP2iActivity extends SDLActivity {
    private static final int REQ_ISO = 0x5101, REQ_FOLDER = 0x5102, REQ_STORAGE = 0x5103, REQ_ALL_FILES = 0x5104;
    private static PSP2iActivity sSelf;
    private static CountDownLatch sLatch;
    private static Uri sPicked;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sSelf = this;
        super.onCreate(savedInstanceState);
    }

    /* Explain, open the picker, and wait for what the user chose (null: declined). */
    private static Uri ask(final String title, final String message, final boolean folder) {
        final PSP2iActivity a = sSelf;
        if (a == null) return null;
        final CountDownLatch latch = new CountDownLatch(1);
        sLatch = latch;
        sPicked = null;
        a.runOnUiThread(() -> new AlertDialog.Builder(a)
            .setTitle(title)
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton("Choose", (d, w) -> {
                Intent i = new Intent(folder ? Intent.ACTION_OPEN_DOCUMENT_TREE : Intent.ACTION_OPEN_DOCUMENT);
                if (!folder) {
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("*/*");
                }
                try {
                    a.startActivityForResult(i, folder ? REQ_FOLDER : REQ_ISO);
                } catch (Exception e) {
                    latch.countDown();
                }
            })
            .setNegativeButton("Quit", (d, w) -> latch.countDown())
            .show());
        try {
            latch.await();
        } catch (InterruptedException e) {
            return null;
        }
        return sPicked;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQ_ALL_FILES) {                 // back from the "All files access" screen
            if (sLatch != null) sLatch.countDown();
            return;
        }
        if (requestCode == REQ_ISO || requestCode == REQ_FOLDER) {
            if (resultCode == RESULT_OK && data != null) sPicked = data.getData();
            if (sLatch != null) sLatch.countDown();
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQ_STORAGE) {
            if (sLatch != null) sLatch.countDown();
            return;
        }
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
    }

    /* ---- the data folder ------------------------------------------------------------- */

    private static String sData;

    private static boolean haveStorage(PSP2iActivity a) {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return a.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    /* Explain, ask for the storage permission (Android 11+: the "All files
     * access" screen; before: the runtime permission), wait, and say whether
     * it is now granted. */
    private static boolean askStorage(final PSP2iActivity a, final String folder) {
        final CountDownLatch latch = new CountDownLatch(1);
        sLatch = latch;
        a.runOnUiThread(() -> new AlertDialog.Builder(a)
            .setTitle("Storage")
            .setMessage("PSP2i keeps its settings (psp2i.ini), saves, fonts and log in\n\n" + folder
                        + "\n\nso you can open them with a file manager or over USB. Android needs "
                        + (Build.VERSION.SDK_INT >= 30 ? "\"All files access\" for this; turn it on for PSP2i on the next screen."
                                                       : "the storage permission for this.")
                        + "\n\nWithout it the files stay in the app's private folder, which most file managers cannot open.")
            .setCancelable(false)
            .setPositiveButton("Allow", (d, w) -> {
                try {
                    if (Build.VERSION.SDK_INT >= 30) {
                        Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                              Uri.parse("package:" + a.getPackageName()));
                        a.startActivityForResult(i, REQ_ALL_FILES);
                    } else {
                        a.requestPermissions(new String[] { Manifest.permission.WRITE_EXTERNAL_STORAGE,
                                                            Manifest.permission.READ_EXTERNAL_STORAGE }, REQ_STORAGE);
                    }
                } catch (Exception e) {
                    latch.countDown();
                }
            })
            .setNegativeButton("Not now", (d, w) -> latch.countDown())
            .show());
        try {
            latch.await();
        } catch (InterruptedException e) {
            return false;
        }
        return haveStorage(a);
    }

    /* Files of src that dst lacks, copied (the originals stay). */
    private static void copyMissing(File src, File dst) {
        File[] list = src.listFiles();
        if (list == null) return;
        for (File f : list) {
            File out = new File(dst, f.getName());
            if (f.isDirectory()) {
                if (out.isDirectory() || out.mkdirs()) copyMissing(f, out);
            } else if (!out.exists()) {
                try (InputStream in = new FileInputStream(f); OutputStream o = new FileOutputStream(out)) {
                    byte[] buf = new byte[1 << 16];
                    int n;
                    while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                } catch (Exception e) {
                    out.delete();                       // no half-copied file
                }
            }
        }
    }

    /* The data folder (see the top). The first time: the permission, and the
     * files of the old private folder copied over. Declined: the private
     * folder, and the player is told where it is. */
    public static String dataFolder() {
        if (sData != null) return sData;
        final PSP2iActivity a = sSelf;
        if (a == null) return null;
        final File priv = a.getExternalFilesDir(null);
        final File pub = new File(Environment.getExternalStorageDirectory(), "PSP2i");
        if ((haveStorage(a) || askStorage(a, pub.getAbsolutePath())) && (pub.isDirectory() || pub.mkdirs())) {
            if (priv != null) copyMissing(priv, pub);
            sData = pub.getAbsolutePath();
            return sData;
        }
        sData = priv != null ? priv.getAbsolutePath() : null;
        if (sData != null) {
            final CountDownLatch latch = new CountDownLatch(1);
            final String where = sData;
            a.runOnUiThread(() -> new AlertDialog.Builder(a)
                .setTitle("Storage")
                .setMessage("PSP2i's files are in\n\n" + where + "\n\n(reachable over USB). To keep them in "
                            + pub.getAbsolutePath() + " instead, give PSP2i storage access in Android's settings "
                            + "and start it again; they are copied over then.")
                .setCancelable(false)
                .setPositiveButton("OK", (d, w) -> latch.countDown())
                .show());
            try {
                latch.await();
            } catch (InterruptedException e) {
                // go on
            }
        }
        return sData;
    }

    /* The modern multiplayer server: "host" (this phone hosts) or the hosting
     * player's address. Asked at every start, prefilled with the last answer;
     * `here` is this device's address, for the players who join it. */
    public static String askModernServer(final String here) {
        final PSP2iActivity a = sSelf;
        if (a == null) return null;
        final SharedPreferences prefs = a.getSharedPreferences("psp2i", MODE_PRIVATE);
        final String last = prefs.getString("modern_server", "host");
        final String[] answer = { last };
        final CountDownLatch latch = new CountDownLatch(1);
        a.runOnUiThread(() -> {
            final android.widget.EditText edit = new android.widget.EditText(a);
            edit.setSingleLine(true);
            edit.setText(last);
            edit.setSelection(last.length());
            new AlertDialog.Builder(a)
                .setTitle("Multiplayer server")
                .setMessage("Ad hoc multiplayer (modern): type host to host on this device, or the address of the "
                            + "player hosting (shown in their settings menu under MODERN SERVER)."
                            + (here != null && !here.isEmpty()
                               ? "\n\nTo host here, the others type: " + here
                                 + "\n(players outside this network also need TCP 27330-27331 forwarded to this device)"
                               : ""))
                .setView(edit)
                .setCancelable(false)
                .setPositiveButton("OK", (d, w) -> {
                    String t = edit.getText().toString().trim();
                    answer[0] = t.isEmpty() ? "host" : t;
                    prefs.edit().putString("modern_server", answer[0]).apply();
                    latch.countDown();
                })
                .show();
        });
        try {
            latch.await();
        } catch (InterruptedException e) {
            return last;
        }
        return answer[0];
    }

    /* A file descriptor for the game's ISO (the caller owns it), or -1. */
    public static int openGameImage(boolean choose) {
        final PSP2iActivity a = sSelf;
        if (a == null) return -1;
        SharedPreferences prefs = a.getSharedPreferences("psp2i", MODE_PRIVATE);
        String saved = prefs.getString("iso", null);
        Uri uri = !choose && saved != null ? Uri.parse(saved) : null;
        for (int attempt = 0; attempt < 2; attempt++) {
            if (uri == null) {
                uri = ask("Game ISO", "Choose the game's disc image (.iso) of Phantasy Star Portable 2 Infinity.", false);
                if (uri == null) return -1;
                try {
                    a.getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception e) {
                    // not persistable from this provider: it is asked for again next time
                }
                prefs.edit().putString("iso", uri.toString()).apply();
            }
            try {
                ParcelFileDescriptor pfd = a.getContentResolver().openFileDescriptor(uri, "r");
                if (pfd != null) return pfd.detachFd();
            } catch (Exception e) {
                // moved or deleted since: ask again
            }
            uri = null;
        }
        return -1;
    }

    /* Copy flash/ from a GameData folder the user picks to <files>/GameData/flash. */
    public static boolean copyFonts() {
        final PSP2iActivity a = sSelf;
        if (a == null) return false;
        Uri tree = ask("PSP fonts",
            "Choose your GameData folder (or its flash folder). The PSP fonts in flash/ are copied into the app once.", true);
        if (tree == null) return false;
        ContentResolver cr = a.getContentResolver();
        String rootId = DocumentsContract.getTreeDocumentId(tree);
        String flashId = rootId;
        if (findChild(cr, tree, rootId, "font") == null) {      // GameData was picked, not flash itself
            flashId = findChild(cr, tree, rootId, "flash");
            if (flashId == null) return false;
        }
        String base = dataFolder();
        File dst = new File(base != null ? new File(base) : a.getExternalFilesDir(null), "GameData/flash");
        if (!dst.isDirectory() && !dst.mkdirs()) return false;
        return copyTree(cr, tree, flashId, dst);
    }

    private static final String[] COLUMNS = {
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE
    };

    private static String findChild(ContentResolver cr, Uri tree, String parentId, String name) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        try (Cursor c = cr.query(children, COLUMNS, null, null, null)) {
            while (c != null && c.moveToNext())
                if (name.equalsIgnoreCase(c.getString(1)) && Document.MIME_TYPE_DIR.equals(c.getString(2)))
                    return c.getString(0);
        } catch (Exception e) {
            return null;
        }
        return null;
    }

    private static boolean copyTree(ContentResolver cr, Uri tree, String parentId, File dst) {
        boolean any = false;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        try (Cursor c = cr.query(children, COLUMNS, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                File out = new File(dst, name);
                if (Document.MIME_TYPE_DIR.equals(mime)) {
                    if (out.isDirectory() || out.mkdirs()) any |= copyTree(cr, tree, id, out);
                    continue;
                }
                try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id));
                     OutputStream o = new FileOutputStream(out)) {
                    byte[] buf = new byte[1 << 16];
                    int n;
                    while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                    any = true;
                }
            }
        } catch (Exception e) {
            return any;
        }
        return any;
    }
}
