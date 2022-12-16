# Standalone Userspace Image Convertor

## Setup

We have provided a [tool](https://github.com/containerd/accelerated-container-image/blob/main/docs/USERSPACE_CONVERTOR.md) to convert OCIv1 images into overlaybd format in userspace. If you are about to use it, we recommend you use our [customized libext2fs](https://github.com/data-accelerator/e2fsprogs) for faster conversion.

You only need to add `-D CUSTOM_EXT2FS=1` when you build [overlaybd](https://github.com/containerd/overlaybd).

```bash
cmake -D CUSTOM_EXT2FS=1 ..
```

## Performance

We used `standalone userspace image-convertor (with custom libext2fs)`, `standalone userspace image-convertor (with origin libext2fs)` and `embedded image-convertor` to convert some images and did a comparison for reference.

| Image               | Image Size | with custom libext2fs | with origin libext2fs | embedded image-convertor |
|:-------------------:|:----------:|:---------------------:|:---------------------:|:------------------------:|
| jupyter-notebook    | 4.84 GB    | 93 s                  | 238 s                 | 101 s                    |
| php-laravel-nginx   | 567 MB     | 13 s                  | 20 s                  | 15 s                     |
| ai-cat-or-dog       | 1.81 GB    | 27 s                  | 54 s                  | 60 s                     |
| cypress-chrome      | 2.73 GB    | 70 s                  | 212 s                 | 87 s                     |
