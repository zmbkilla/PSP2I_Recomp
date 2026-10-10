package com.psp2i.recomp;

import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;

import java.io.File;
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
 *                  GameData folder the user picks, into the app's own files
 *
 * Both show their dialog on the UI thread and wait for the answer. */
public class PSP2iActivity extends SDLActivity {
    private static final int REQ_ISO = 0x5101, REQ_FOLDER = 0x5102;
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
        if (requestCode == REQ_ISO || requestCode == REQ_FOLDER) {
            if (resultCode == RESULT_OK && data != null) sPicked = data.getData();
            if (sLatch != null) sLatch.countDown();
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    /* The modern multiplayer server: "host" (this phone hosts) or the hosting
     * player's address. Asked at every start, prefilled with the last answer. */
    public static String askModernServer() {
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
                            + "player hosting (shown in their settings menu under MODERN SERVER).")
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
        File dst = new File(a.getExternalFilesDir(null), "GameData/flash");
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
