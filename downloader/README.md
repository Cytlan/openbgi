Downloader
==========

This tool assists with downloading and extracting *trial versions* of various Lump of Sugar games that can be used when reverse-engineering the Buriko General Interpreter engine.

The download links were found on Lump of Sugar's own website.

If you want to add games and/or mirrors, please make sure they're from officially blessed sources.

Setup
-----

Before you can run the downloader, you need to install the dependencies.

You can install the dependencies running this command in the downloader directory:
```sh
npm install
```

Running
-------

Running it is as simple as typing:
```sh
nodejs download.js
```

It should immediately start downloading games and extract them.

Note: You only need to run the downloader once. Re-running the downloader will delete all the extracted files and re-extract them (useful if you messed up some files).

Games
-----

Currently, these games are included:

```
0307b275280d41f59a1c28b5b54851a2365959f5292e5877ab7e5ebfcb6d1227 | nurseryrhyme_trial.lzh        | Nursery Rhyme trial
9d2edb1e6efa35fef17b70c1dc7b2424074d97c2e9d1165139b171230ec3e783 | tayutama_trial_Web.zip        | タユタマ (Tayutama) trial
56140f676eacdb539db5d7f071fc01462afb81608940e043075f2ff83866083b | HanairoHeptagram_WebTrial.zip | 花色ヘプタグラム (Color of the flower Heptagram) trial
074539c57fd78c5e54aa8aa3197ef58bd27feaf16a01e35f767e37dc8bf6e2f7 | tayutama2_trial_EX.zip        | タユタマ２ (Tayutama 2) trial
```
