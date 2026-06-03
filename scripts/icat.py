# This is a compiled file.
# For the original source, see https://github.com/trygveaa/weechat-icat
# Vendored in xepher under the MIT license (Copyright 2023 Trygve Aaberge)

from __future__ import annotations
from base64 import b64decode, b64encode
from collections import defaultdict
from dataclasses import dataclass
from dataclasses import dataclass, field
from io import StringIO
from PIL import Image
from random import randint
from typing import Callable
from typing import Callable, Dict
from typing import Callable, Dict, List, Optional, Union
from typing import Callable, Dict, Union
from typing import Dict, List, Optional
from uuid import uuid4
from uuid import UUID, uuid4
import array
import fcntl
import io
import os
import pickle
import PIL
import re
import sys
import termios
import weechat




def get_callback_name(callback: Callable[..., WeechatCallbackReturnType]) -> str:
    callback_id = f"{callback.__name__}-{id(callback)}"
    shared.weechat_callbacks[callback_id] = callback
    return callback_id




downloaded_images: Dict[str, str] = {}
image_placements: Dict[str, List[ImagePlacement]] = defaultdict(list)


@dataclass
class ImageDownloadedData:
    buffer: str
    url: str
    path: str
    columns: Optional[int]
    rows: Optional[int]
    print_immediately: bool


@dataclass
class ImageCreatedData:
    buffer: str
    path: str
    print_immediately: bool


def parse_options(args: str, supported_options: Dict[str, bool]):
    args_split = re.split(r"(\s+)", args)
    pos_args: List[str] = []
    options: Dict[str, str] = {}
    i = 0
    while i < len(args_split):
        arg = args_split[i]
        option_name = removeprefix(arg, "-")
        if option_name in supported_options:
            if supported_options[option_name]:
                i += 2
                options[option_name] = args_split[i]
            else:
                options[option_name] = " "
            i += 1
        else:
            pos_args.append(arg)
        i += 1
    return "".join(pos_args).strip(), options


def new_image_placement(buffer: str, image_placement: ImagePlacement):
    display_image(buffer, image_placement)
    image_placements[image_placement.path].append(image_placement)


def image_downloaded_cb(data_serialized: str):
    data: ImageDownloadedData = pickle.loads(b64decode(data_serialized))
    downloaded_images[data.url] = data.path
    create_image(
        data.buffer, data.path, data.columns, data.rows, data.print_immediately
    )


def image_created_cb(
    data_serialized: str,
    result: ImageCreateFinished,
    image_placement_was_returned: bool,
):
    data: ImageCreatedData = pickle.loads(b64decode(data_serialized))

    if isinstance(result, Exception):
        if image_placement_was_returned:
            del image_placements[data.path]

        if isinstance(result, PIL.UnidentifiedImageError):
            print_error("failed to load image")
            return

        print_error("failed displaying image:")
        raise result

    if data.print_immediately and image_placement_was_returned:
        weechat.command(data.buffer, "/window refresh")
    else:
        new_image_placement(data.buffer, result)


def images_restored_cb(buffer: str, result: ImagesSendFinished):
    if isinstance(result, Exception):
        print_error("failed restoring images:")
        raise result

    weechat.command(buffer, "/window refresh")
    print_info("finished restoring images")


def download_and_create_image(
    buffer: str,
    url: str,
    columns: Optional[int],
    rows: Optional[int],
    print_immediately: bool,
):
    downloaded_path = downloaded_images.get(url)
    if downloaded_path and os.path.isfile(downloaded_path):
        create_image(
            buffer,
            downloaded_path,
            columns,
            rows,
            bool(print_immediately),
        )
    else:
        save_path = weechat.string_eval_path_home(
            f"{shared.cache_downloaded_images_path}/{uuid4()}", {}, {}, {}
        )
        image_downloaded_data = ImageDownloadedData(
            buffer,
            url,
            save_path,
            columns,
            rows,
            bool(print_immediately),
        )
        callback_data = b64encode(pickle.dumps(image_downloaded_data)).decode("ascii")
        download_image(url, save_path, image_downloaded_cb, callback_data)


def create_image(
    buffer: str,
    path: str,
    columns: Optional[int],
    rows: Optional[int],
    print_immediately: bool,
):
    for ip in image_placements[path]:
        if (columns is None or columns == ip.columns) and (
            rows is None or rows == ip.rows
        ):
            image_placement = ip
            display_image(buffer, image_placement)
            break
    else:
        image_created_data = ImageCreatedData(buffer, path, print_immediately)
        callback_data = b64encode(pickle.dumps(image_created_data)).decode("ascii")
        image_placement = create_and_send_image_to_terminal(
            path, columns, rows, image_created_cb, callback_data
        )
        if print_immediately and image_placement:
            new_image_placement(buffer, image_placement)


def icat_cb(data: str, buffer: str, args: str) -> int:
    pos_args, options = parse_options(
        args,
        {
            "columns": True,
            "rows": True,
            "print_immediately": False,
            "restore": False,
            "quiet": False,
        },
    )
    shared.print_errors = not options.get("quiet")
    if "restore" in options:
        image_placements_values = [
            image_placement
            for image_placement_list in image_placements.values()
            for image_placement in image_placement_list
        ]
        send_images_to_terminal(image_placements_values, images_restored_cb, buffer)
    else:
        columns = options.get("columns")
        if columns is not None and not columns.isdecimal():
            print_error("columns must be a positive integer")
            return weechat.WEECHAT_RC_ERROR
        columns_int = int(columns) if columns else None

        rows = options.get("rows")
        if rows is not None and not rows.isdecimal():
            print_error("rows must be a positive integer")
            return weechat.WEECHAT_RC_ERROR
        rows_int = int(rows) if rows else None

        print_immediately = options.get("print_immediately")
        if print_immediately and (not columns_int or not rows_int):
            print_error(
                "both -columns and -rows must be specified when using -print_immediately"
            )
            return weechat.WEECHAT_RC_ERROR

        path_or_url = weechat.string_eval_path_home(pos_args, {}, {}, {})
        if path_or_url.startswith(("http://", "https://")):
            download_and_create_image(
                buffer, path_or_url, columns_int, rows_int, bool(print_immediately)
            )
        elif os.path.isfile(path_or_url):
            create_image(
                buffer, path_or_url, columns_int, rows_int, bool(print_immediately)
            )
        else:
            print_error("filename must point to an existing file")
            return weechat.WEECHAT_RC_ERROR

    return weechat.WEECHAT_RC_OK


def register_commands():
    command_icat_description = (
        "          -columns: number of columns to use to display the image\n"
        "             -rows: number of rows to use to display the image\n"
        "-print_immediately: print the image lines immediately (the lines will "
        "be blank until the image is created); requires both -columns and -rows\n"
        "          filename: image to display\n"
        "            -quiet: don't print any error messages\n"
        "          -restore: instead of displaying a new image, restore the existing "
        "images to a new terminal instance\n"
        "\n"
        "Note that images are loaded in the background, so they may not be "
        "displayed immediately after running the command."
    )
    weechat.hook_command(
        "icat",
        "display an image in the chat",
        "[-columns <columns>] [-rows <rows>] [-print_immediately] [-quiet] <filename> || -restore [-quiet]",
        command_icat_description,
        "-columns|-rows|-print_immediately|-quiet|%* || -restore|-quiet|%*",
        get_callback_name(icat_cb),
        "",
    )




string_buffers: Dict[str, StringIO] = defaultdict(StringIO)


@dataclass
class DownloadImageData:
    callback: Callable[[str], None]
    callback_data: str
    uuid: UUID = uuid4()


def download_image_cb(
    data_serialized: str, command: str, return_code: int, out_chunk: str, err_chunk: str
) -> int:
    data: DownloadImageData = pickle.loads(b64decode(data_serialized))
    err_key = f"{str(data.uuid)}_err"
    string_buffers[err_key].write(err_chunk)

    if return_code == -1:
        return weechat.WEECHAT_RC_OK

    err = string_buffers[err_key].getvalue()
    string_buffers[err_key].close()
    del string_buffers[err_key]

    if return_code == weechat.WEECHAT_HOOK_PROCESS_ERROR or return_code > 0 or err:
        print_error(f"failed downloading image, return_code={return_code}, err='{err}'")
        return weechat.WEECHAT_RC_OK

    data.callback(data.callback_data)
    return weechat.WEECHAT_RC_OK


def download_image(
    url: str, save_path: str, callback: Callable[[str], None], callback_data: str
):
    data = DownloadImageData(callback, callback_data)
    data_serialized = b64encode(pickle.dumps(data)).decode("ascii")
    weechat.hook_process_hashtable(
        f"url:{url}",
        {"file_out": save_path},
        60000,
        get_callback_name(download_image_cb),
        data_serialized,
    )




@dataclass
class ImageData:
    data: bytes
    width: int
    height: int


def load_image_data(path: str):
    with io.BytesIO() as data:
        with Image.open(path) as im:
            im.save(data, "png")
            return ImageData(data.getvalue(), im.width, im.height)




def print_log(prefix: str, message: str):
    sep = "" if prefix.endswith("\t") else "\t"
    weechat.prnt("", f"{prefix}{sep}{shared.SCRIPT_NAME}: {message}")


def print_info(message: str):
    print_log("", message)


def print_error(message: str):
    if shared.print_errors:
        print_log(weechat.prefix("error"), message)


# Copied from https://peps.python.org/pep-0616/ for support for Python < 3.9
def removeprefix(self: str, prefix: str) -> str:
    if self.startswith(prefix):
        return self[len(prefix) :]
    else:
        return self[:]


# Copied from https://peps.python.org/pep-0616/ for support for Python < 3.9
def removesuffix(self: str, suffix: str) -> str:
    if suffix and self.endswith(suffix):
        return self[: -len(suffix)]
    else:
        return self[:]



SCRIPT_AUTHOR = "Trygve Aaberge <trygveaa@gmail.com>"
SCRIPT_LICENSE = "MIT"
SCRIPT_DESC = "Display images in the chat"


def create_cache_paths():
    paths = [shared.cache_path, shared.cache_downloaded_images_path]
    for path in paths:
        if not weechat.mkdir_home(path, 0o755):
            raise RuntimeError("Failed creating cache path")


def register():
    if weechat.register(
        shared.SCRIPT_NAME,
        SCRIPT_AUTHOR,
        shared.SCRIPT_VERSION,
        SCRIPT_LICENSE,
        SCRIPT_DESC,
        "",
        "",
    ):
        create_cache_paths()
        register_commands()


WeechatCallbackReturnType = Union[int, str, Dict[str, str], None]


class Shared:
    def __init__(self):
        self.SCRIPT_NAME = "icat"
        self.SCRIPT_VERSION = "0.1.0"

        self.weechat_callbacks: Dict[str, Callable[..., WeechatCallbackReturnType]]
        self.cache_path = "${weechat_cache_dir}/icat"
        self.cache_downloaded_images_path = f"{self.cache_path}/downloaded_images"
        self.print_errors = True


shared = Shared()

rowcolumn_diacritics_chars = [
    "\u0305",
    "\u030D",
    "\u030E",
    "\u0310",
    "\u0312",
    "\u033D",
    "\u033E",
    "\u033F",
    "\u0346",
    "\u034A",
    "\u034B",
    "\u034C",
    "\u0350",
    "\u0351",
    "\u0352",
    "\u0357",
    "\u035B",
    "\u0363",
    "\u0364",
    "\u0365",
    "\u0366",
    "\u0367",
    "\u0368",
    "\u0369",
    "\u036A",
    "\u036B",
    "\u036C",
    "\u036D",
    "\u036E",
    "\u036F",
    "\u0483",
    "\u0484",
    "\u0485",
    "\u0486",
    "\u0487",
    "\u0592",
    "\u0593",
    "\u0594",
    "\u0595",
    "\u0597",
    "\u0598",
    "\u0599",
    "\u059C",
    "\u059D",
    "\u059E",
    "\u059F",
    "\u05A0",
    "\u05A1",
    "\u05A8",
    "\u05A9",
    "\u05AB",
    "\u05AC",
    "\u05AF",
    "\u05C4",
    "\u0610",
    "\u0611",
    "\u0612",
    "\u0613",
    "\u0614",
    "\u0615",
    "\u0616",
    "\u0617",
    "\u0657",
    "\u0658",
    "\u0659",
    "\u065A",
    "\u065B",
    "\u065D",
    "\u065E",
    "\u06D6",
    "\u06D7",
    "\u06D8",
    "\u06D9",
    "\u06DA",
    "\u06DB",
    "\u06DC",
    "\u06DF",
    "\u06E0",
    "\u06E1",
    "\u06E2",
    "\u06E4",
    "\u06E7",
    "\u06E8",
    "\u06EB",
    "\u06EC",
    "\u0730",
    "\u0732",
    "\u0733",
    "\u0735",
    "\u0736",
    "\u073A",
    "\u073D",
    "\u073F",
    "\u0740",
    "\u0741",
    "\u0743",
    "\u0745",
    "\u0747",
    "\u0749",
    "\u074A",
    "\u07EB",
    "\u07EC",
    "\u07ED",
    "\u07EE",
    "\u07EF",
    "\u07F0",
    "\u07F1",
    "\u07F3",
    "\u0816",
    "\u0817",
    "\u0818",
    "\u0819",
    "\u081B",
    "\u081C",
    "\u081D",
    "\u081E",
    "\u081F",
    "\u0820",
    "\u0821",
    "\u0822",
    "\u0823",
    "\u0825",
    "\u0826",
    "\u0827",
    "\u0829",
    "\u082A",
    "\u082B",
    "\u082C",
    "\u082D",
    "\u0951",
    "\u0953",
    "\u0954",
    "\u0F82",
    "\u0F83",
    "\u0F86",
    "\u0F87",
    "\u135D",
    "\u135E",
    "\u135F",
    "\u17DD",
    "\u193A",
    "\u1A17",
    "\u1A75",
    "\u1A76",
    "\u1A77",
    "\u1A78",
    "\u1A79",
    "\u1A7A",
    "\u1A7B",
    "\u1A7C",
    "\u1B6B",
    "\u1B6D",
    "\u1B6E",
    "\u1B6F",
    "\u1B70",
    "\u1B71",
    "\u1B72",
    "\u1B73",
    "\u1CD0",
    "\u1CD1",
    "\u1CD2",
    "\u1CDA",
    "\u1CDB",
    "\u1CE0",
    "\u1DC0",
    "\u1DC1",
    "\u1DC3",
    "\u1DC4",
    "\u1DC5",
    "\u1DC6",
    "\u1DC7",
    "\u1DC8",
    "\u1DC9",
    "\u1DCB",
    "\u1DCC",
    "\u1DD1",
    "\u1DD2",
    "\u1DD3",
    "\u1DD4",
    "\u1DD5",
    "\u1DD6",
    "\u1DD7",
    "\u1DD8",
    "\u1DD9",
    "\u1DDA",
    "\u1DDB",
    "\u1DDC",
    "\u1DDD",
    "\u1DDE",
    "\u1DDF",
    "\u1DE0",
    "\u1DE1",
    "\u1DE2",
    "\u1DE3",
    "\u1DE4",
    "\u1DE5",
    "\u1DE6",
    "\u1DFE",
    "\u20D0",
    "\u20D1",
    "\u20D4",
    "\u20D5",
    "\u20D6",
    "\u20D7",
    "\u20DB",
    "\u20DC",
    "\u20E1",
    "\u20E7",
    "\u20E9",
    "\u20F0",
    "\u2CEF",
    "\u2CF0",
    "\u2CF1",
    "\u2DE0",
    "\u2DE1",
    "\u2DE2",
    "\u2DE3",
    "\u2DE4",
    "\u2DE5",
    "\u2DE6",
    "\u2DE7",
    "\u2DE8",
    "\u2DE9",
    "\u2DEA",
    "\u2DEB",
    "\u2DEC",
    "\u2DED",
    "\u2DEE",
    "\u2DEF",
    "\u2DF0",
    "\u2DF1",
    "\u2DF2",
    "\u2DF3",
    "\u2DF4",
    "\u2DF5",
    "\u2DF6",
    "\u2DF7",
    "\u2DF8",
    "\u2DF9",
    "\u2DFA",
    "\u2DFB",
    "\u2DFC",
    "\u2DFD",
    "\u2DFE",
    "\u2DFF",
    "\uA66F",
    "\uA67C",
    "\uA67D",
    "\uA6F0",
    "\uA6F1",
    "\uA8E0",
    "\uA8E1",
    "\uA8E2",
    "\uA8E3",
    "\uA8E4",
    "\uA8E5",
    "\uA8E6",
    "\uA8E7",
    "\uA8E8",
    "\uA8E9",
    "\uA8EA",
    "\uA8EB",
    "\uA8EC",
    "\uA8ED",
    "\uA8EE",
    "\uA8EF",
    "\uA8F0",
    "\uA8F1",
    "\uAAB0",
    "\uAAB2",
    "\uAAB3",
    "\uAAB7",
    "\uAAB8",
    "\uAABE",
    "\uAABF",
    "\uAAC1",
    "\uFE20",
    "\uFE21",
    "\uFE22",
    "\uFE23",
    "\uFE24",
    "\uFE25",
    "\uFE26",
    "\U00010A0F",
    "\U00010A38",
    "\U0001D185",
    "\U0001D186",
    "\U0001D187",
    "\U0001D188",
    "\U0001D189",
    "\U0001D1AA",
    "\U0001D1AB",
    "\U0001D1AC",
    "\U0001D1AD",
    "\U0001D242",
    "\U0001D243",
    "\U0001D244",
]




string_buffers: Dict[str, StringIO] = defaultdict(StringIO)
image_create_queue: List[ImageCreateData] = []


@dataclass
class TerminalSize:
    rows: int
    columns: int
    width: int
    height: int


@dataclass
class ImagePlacement:
    path: str
    image_id: int
    columns: int
    rows: int
    terminal_cmds: List[bytes] = field(default_factory=list)


ImageCreateFinished = Union[ImagePlacement, Exception]
ImagesSendFinished = Union[None, Exception]


@dataclass
class ImageCreateData:
    path: str
    image_id: int
    columns: Optional[int]
    rows: Optional[int]
    terminal_size: TerminalSize
    image_placement: Optional[ImagePlacement]
    callback: Callable[[str, ImageCreateFinished, bool], None]
    callback_data: str
    uuid: UUID = uuid4()


@dataclass
class ImagesSendData:
    image_placements: List[ImagePlacement]
    callback: Callable[[str, ImagesSendFinished], None]
    callback_data: str
    uuid: UUID = uuid4()


def get_terminal_size():
    buf = array.array("H", [0, 0, 0, 0])
    fcntl.ioctl(1, termios.TIOCGWINSZ, buf)
    return TerminalSize(*buf)


def get_random_image_id():
    image_id_upper = randint(0, 255)
    image_id_lower = randint(0, 255)
    return (image_id_upper << 24) + image_id_lower


def serialize_gr_command(control_data: Dict[str, Union[str, int]], payload: bytes):
    tmux = weechat.string_eval_expression("${env:TMUX}", {}, {}, {})
    esc = b"\033\033" if tmux else b"\033"
    control_data_str = ",".join(f"{k}={v}" for k, v in control_data.items())
    ans = [
        b"\033Ptmux;" if tmux else b"",
        esc + b"_G",
        control_data_str.encode("ascii"),
        b";" + payload if payload else b"",
        esc + b"\\",
        b"\033\\" if tmux else b"",
    ]
    return b"".join(ans)


def write_chunked(control_data: Dict[str, Union[str, int]], data: bytes):
    cmds: List[bytes] = []
    with open(os.ctermid(), "wb") as tty:
        data_base64 = b64encode(data)
        while data_base64:
            chunk, data_base64 = data_base64[:4096], data_base64[4096:]
            m = 1 if data_base64 else 0
            cmd = serialize_gr_command({**control_data, "m": m}, chunk)
            cmds.append(cmd)
            tty.write(cmd)
            tty.flush()
            control_data.clear()
    return cmds


def get_cell_character(
    image_id: int,
    y: int,
    x: int,
    include_color: bool = False,
):
    image_id_upper = image_id >> 24
    image_id_lower = image_id & 0xFF
    color = weechat.color(str(image_id_lower)) if include_color else ""
    y_char = rowcolumn_diacritics_chars[y]
    x_char = rowcolumn_diacritics_chars[x]
    id_char = rowcolumn_diacritics_chars[image_id_upper]
    return f"{color}\U0010eeee{y_char}{x_char}{id_char}"


def create_and_send_image_to_terminal_bg(data_serialized: str) -> str:
    try:
        data: ImageCreateData = pickle.loads(b64decode(data_serialized))
        image_data = load_image_data(data.path)

        if data.image_placement:
            image_placement = data.image_placement
        else:
            image_columns = (
                image_data.width / data.terminal_size.width * data.terminal_size.columns
            )
            image_rows = (
                image_data.height / data.terminal_size.height * data.terminal_size.rows
            )

            if not data.columns:
                rows = data.rows or 5
                columns = rows / image_rows * image_columns
            else:
                columns = data.columns
                rows = data.rows or columns / image_columns * image_rows

            image_placement = ImagePlacement(
                data.path, data.image_id, round(columns), round(rows)
            )

        send_image_to_terminal(image_placement, image_data)

        return b64encode(pickle.dumps(image_placement)).decode("ascii")
    except Exception as e:  # pylint: disable=broad-exception-caught
        return b64encode(pickle.dumps(e)).decode("ascii")


def create_and_send_image_to_terminal_bg_finished_cb(
    data_serialized: str, command: str, return_code: int, out_chunk: str, err_chunk: str
) -> int:
    try:
        data: ImageCreateData = pickle.loads(b64decode(data_serialized))
        out_key = f"{str(data.uuid)}_out"
        err_key = f"{str(data.uuid)}_err"
        string_buffers[out_key].write(out_chunk)
        string_buffers[err_key].write(err_chunk)

        if return_code == -1:
            return weechat.WEECHAT_RC_OK

        out = string_buffers[out_key].getvalue()
        err = string_buffers[err_key].getvalue()
        string_buffers[out_key].close()
        string_buffers[err_key].close()
        del string_buffers[out_key]
        del string_buffers[err_key]

        image_placement_was_returned = data.image_placement is not None

        if return_code == weechat.WEECHAT_HOOK_PROCESS_ERROR or return_code > 0 or err:
            error = RuntimeError(f"return_code={return_code}, err='{err}'")
            data.callback(data.callback_data, error, image_placement_was_returned)
            return weechat.WEECHAT_RC_OK

        result: ImageCreateFinished = pickle.loads(b64decode(out))
        data.callback(data.callback_data, result, image_placement_was_returned)
    finally:
        if return_code != -1:
            image_create_queue.pop(0)
            start_image_create_job(True)
    return weechat.WEECHAT_RC_OK


def start_image_create_job(prev_job_finished: bool):
    if image_create_queue and prev_job_finished or len(image_create_queue) == 1:
        data_serialized = b64encode(pickle.dumps(image_create_queue[0])).decode("ascii")
        weechat.hook_process(
            "func:" + get_callback_name(create_and_send_image_to_terminal_bg),
            60000,
            get_callback_name(create_and_send_image_to_terminal_bg_finished_cb),
            data_serialized,
        )


def create_and_send_image_to_terminal(
    image_path: str,
    columns: Optional[int],
    rows: Optional[int],
    callback: Callable[[str, ImageCreateFinished, bool], None],
    callback_data: str,
):
    image_id = get_random_image_id()
    if columns and rows:
        image_placement = ImagePlacement(image_path, image_id, columns, rows)
    else:
        image_placement = None

    image_create_data = ImageCreateData(
        image_path,
        image_id,
        columns,
        rows,
        get_terminal_size(),
        image_placement,
        callback,
        callback_data,
    )
    image_create_queue.append(image_create_data)
    start_image_create_job(False)
    return image_placement


def send_image_to_terminal(
    image_placement: ImagePlacement, image_data: Optional[ImageData] = None
):
    if image_placement.terminal_cmds:
        with open(os.ctermid(), "wb") as tty:
            for cmd in image_placement.terminal_cmds:
                tty.write(cmd)
                tty.flush()
    else:
        control_data = {
            "a": "T",
            "q": 2,
            "f": 100,
            "U": 1,
            "c": image_placement.columns,
            "r": image_placement.rows,
            "i": image_placement.image_id,
        }
        image_data = load_image_data(image_placement.path)
        image_placement.terminal_cmds = write_chunked(control_data, image_data.data)


def send_images_to_terminal_bg(data_serialized: str):
    try:
        data: ImagesSendData = pickle.loads(b64decode(data_serialized))
        for image_placement in data.image_placements:
            send_image_to_terminal(image_placement)
        return b64encode(pickle.dumps(None)).decode("ascii")
    except Exception as e:  # pylint: disable=broad-exception-caught
        return b64encode(pickle.dumps(e)).decode("ascii")


def send_images_to_terminal_bg_finished_cb(
    data_serialized: str, command: str, return_code: int, out_chunk: str, err_chunk: str
) -> int:
    data: ImagesSendData = pickle.loads(b64decode(data_serialized))
    out_key = f"{str(data.uuid)}_out"
    err_key = f"{str(data.uuid)}_err"
    string_buffers[out_key].write(out_chunk)
    string_buffers[err_key].write(err_chunk)

    if return_code == -1:
        return weechat.WEECHAT_RC_OK

    out = string_buffers[out_key].getvalue()
    err = string_buffers[err_key].getvalue()
    string_buffers[out_key].close()
    string_buffers[err_key].close()
    del string_buffers[out_key]
    del string_buffers[err_key]

    if return_code == weechat.WEECHAT_HOOK_PROCESS_ERROR or return_code > 0 or err:
        error = RuntimeError(f"return_code={return_code}, err='{err}'")
        data.callback(data.callback_data, error)
        return weechat.WEECHAT_RC_OK

    result: ImagesSendFinished = pickle.loads(b64decode(out))
    data.callback(data.callback_data, result)
    return weechat.WEECHAT_RC_OK


def send_images_to_terminal(
    image_placements: List[ImagePlacement],
    callback: Callable[[str, ImagesSendFinished], None],
    callback_data: str,
):
    images_send_data = ImagesSendData(
        image_placements,
        callback,
        callback_data,
    )
    data_serialized = b64encode(pickle.dumps(images_send_data)).decode("ascii")

    weechat.hook_process(
        "func:" + get_callback_name(send_images_to_terminal_bg),
        60000,
        get_callback_name(send_images_to_terminal_bg_finished_cb),
        data_serialized,
    )


def display_image(buffer: str, image_placement: ImagePlacement):
    for y in range(image_placement.rows):
        chars = [
            get_cell_character(image_placement.image_id, y, x, include_color=x == 0)
            for x in range(image_placement.columns)
        ]
        weechat.prnt(buffer, "".join(chars))

sys.path.append(os.path.dirname(os.path.realpath(__file__)))

shared.weechat_callbacks = globals()

if __name__ == "__main__":
    register()
