import asyncio
import websockets
import datetime
import os
import subprocess
from collections import defaultdict
from datetime import timedelta
import logging

# --- Configuration ---
PORT = 8081
PATH = "/upload"
# Match the targetFPS potentially used in ESP32, or desired output FPS
TARGET_FPS = 30
CACHE_DIR = "image_cache"
OUTPUT_DIR = "output_videos"
VIDEO_INTERVAL_SECONDS = 60  # Create video every 60 seconds
FFMPEG_PATH = "ffmpeg"  # Ensure ffmpeg is in PATH or provide full path

# --- Globals ---
# Stores tuples of (timestamp, file_path)
cached_frames = []
# Lock for safely accessing cached_frames if needed in more complex scenarios,
# but likely okay with asyncio's single-threaded nature for I/O bound tasks.
# cache_lock = asyncio.Lock()

# --- Logging ---
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

# --- Functions ---

async def save_frame(websocket, path):
    """Handles incoming WebSocket connections and saves frames."""
    global cached_frames
    logging.info(f"Client connected from {websocket.remote_address}")
    try:
        async for message in websocket:
            if isinstance(message, bytes):
                now = datetime.datetime.now()
                timestamp_str = now.strftime("%Y%m%d_%H%M%S_%f")
                filename = f"frame_{timestamp_str}.jpg"
                file_path = os.path.join(CACHE_DIR, filename)

                try:
                    with open(file_path, "wb") as f:
                        f.write(message)
                    # async with cache_lock: # Use lock if strict thread-safety needed
                    cached_frames.append((now, file_path))
                    logging.debug(f"Received and saved frame: {file_path}, Size: {len(message)} bytes")
                except IOError as e:
                    logging.error(f"Error saving frame {file_path}: {e}")

            else:
                logging.warning(f"Received non-binary message: {message}")
    except websockets.exceptions.ConnectionClosedOK:
        logging.info(f"Client {websocket.remote_address} disconnected normally.")
    except websockets.exceptions.ConnectionClosedError as e:
        logging.warning(f"Client {websocket.remote_address} disconnected with error: {e}")
    except Exception as e:
        logging.error(f"Error in WebSocket handler for {websocket.remote_address}: {e}")
    finally:
        logging.info(f"Connection closed for {websocket.remote_address}")


async def create_video_periodically():
    """Periodically creates a video from cached frames."""
    global cached_frames
    while True:
        await asyncio.sleep(VIDEO_INTERVAL_SECONDS)
        logging.info("Video creation task started.")

        # async with cache_lock: # Use lock if strict thread-safety needed
        current_frames_to_process = list(cached_frames) # Take a snapshot
        cached_frames = [] # Clear global list for the next interval

        if not current_frames_to_process:
            logging.info("No frames cached, skipping video creation.")
            continue

        logging.info(f"Processing {len(current_frames_to_process)} frames for video.")

        # Sort frames by timestamp
        current_frames_to_process.sort(key=lambda x: x[0])

        # Determine the actual time range covered by these frames
        first_ts = current_frames_to_process[0][0]
        last_ts = current_frames_to_process[-1][0]
        duration = (last_ts - first_ts).total_seconds()
        logging.info(f"Frames span from {first_ts} to {last_ts} (Duration: {duration:.2f}s)")

        # Group frames by second relative to the first frame's timestamp
        frames_by_second = defaultdict(list)
        for ts, file_path in current_frames_to_process:
            second_index = int((ts - first_ts).total_seconds())
            frames_by_second[second_index].append(file_path)

        final_frame_list = []
        # Use max(1, ...) to avoid issues if duration is very short
        total_seconds_in_span = max(1, int(duration) + 1)

        # Use the first frame as a fallback if a second has no frames
        last_available_frame = current_frames_to_process[0][1]

        logging.info(f"Adjusting frames per second to {TARGET_FPS} over {total_seconds_in_span} seconds...")
        for sec in range(total_seconds_in_span):
            second_frames = frames_by_second.get(sec, [])
            if not second_frames:
                # No frames this second, repeat the last known frame
                final_frame_list.extend([last_available_frame] * TARGET_FPS)
                logging.debug(f"Second {sec}: No frames, repeating last ({TARGET_FPS} times).")
            elif len(second_frames) < TARGET_FPS:
                # Fewer frames than needed, duplicate the last one of this second
                last_frame_this_second = second_frames[-1]
                final_frame_list.extend(second_frames)
                duplicates_needed = TARGET_FPS - len(second_frames)
                final_frame_list.extend([last_frame_this_second] * duplicates_needed)
                last_available_frame = last_frame_this_second
                logging.debug(f"Second {sec}: {len(second_frames)} frames, duplicating last {duplicates_needed} times.")
            else:
                # Enough or too many frames, take the first TARGET_FPS
                selected_frames = second_frames[:TARGET_FPS]
                final_frame_list.extend(selected_frames)
                last_available_frame = selected_frames[-1] # Update last known frame
                logging.debug(f"Second {sec}: {len(second_frames)} frames, taking first {TARGET_FPS}.")

        if not final_frame_list:
             logging.warning("Final frame list is empty after processing. Skipping FFmpeg.")
             # Cleanup the files that were intended for processing but failed
             files_to_delete = {fp for _, fp in current_frames_to_process}
             cleanup_files(files_to_delete)
             continue

        # Create the list file for ffmpeg concat demuxer
        list_file_path = os.path.join(CACHE_DIR, "framelist.txt")
        try:
            with open(list_file_path, "w") as f:
                for frame_path in final_frame_list:
                    # Use absolute paths and forward slashes for compatibility
                    abs_path = os.path.abspath(frame_path).replace(os.sep, '/')
                    f.write(f"file '{abs_path}'\n")
            logging.info(f"Generated frame list file: {list_file_path}")
        except IOError as e:
            logging.error(f"Error writing frame list file {list_file_path}: {e}")
            # Cleanup the files that were intended for processing but failed
            files_to_delete = {fp for _, fp in current_frames_to_process}
            cleanup_files(files_to_delete)
            continue


        # Generate output video filename based on the time the process runs
        video_filename = f"{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}.mp4"
        output_video_path = os.path.join(OUTPUT_DIR, video_filename)

        # Construct and run FFmpeg command
        ffmpeg_cmd = [
            FFMPEG_PATH,
            "-y",  # Overwrite output file if it exists
            "-f", "concat",
            "-safe", "0",  # Allow absolute paths in list file
            "-r", str(TARGET_FPS), # Treat input images as sequence with this frame rate
            "-i", list_file_path,
            "-c:v", "libx264",  # Video codec
            "-preset", "fast", # Encoding speed/compression trade-off
            "-pix_fmt", "yuv420p",  # Pixel format for broad compatibility
            "-r", str(TARGET_FPS), # Set output frame rate explicitly
            output_video_path,
        ]

        logging.info(f"Running FFmpeg command: {' '.join(ffmpeg_cmd)}")
        try:
            process = await asyncio.create_subprocess_exec(
                *ffmpeg_cmd,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE
            )
            stdout, stderr = await process.communicate()

            if process.returncode == 0:
                logging.info(f"Video created successfully: {output_video_path}")
                # Cleanup processed cache files
                files_to_delete = {fp for _, fp in current_frames_to_process}
                cleanup_files(files_to_delete)
            else:
                logging.error(f"FFmpeg failed with code {process.returncode}")
                logging.error(f"FFmpeg stderr:\n{stderr.decode(errors='ignore')}")
                # Optional: Decide whether to put frames back into cache or discard
                # For now, they are discarded as cached_frames was cleared earlier.
                # Consider adding them back if retry logic is desired:
                # cached_frames.extend(current_frames_to_process)

        except FileNotFoundError:
             logging.error(f"FFmpeg command not found at '{FFMPEG_PATH}'. Please ensure it's installed and in PATH.")
             # Cleanup files as ffmpeg couldn't run
             files_to_delete = {fp for _, fp in current_frames_to_process}
             cleanup_files(files_to_delete)
        except Exception as e:
            logging.error(f"Error running FFmpeg: {e}")
            # Cleanup files as ffmpeg couldn't run
            files_to_delete = {fp for _, fp in current_frames_to_process}
            cleanup_files(files_to_delete)


        # Delete the temporary list file
        try:
            if os.path.exists(list_file_path):
                os.remove(list_file_path)
                logging.debug(f"Deleted frame list file: {list_file_path}")
        except OSError as e:
            logging.error(f"Error deleting list file {list_file_path}: {e}")

        logging.info("Video creation task finished.")


def cleanup_files(file_paths):
    """Safely removes a set of files."""
    logging.info(f"Cleaning up {len(file_paths)} cached files...")
    for file_path in file_paths:
        try:
            if os.path.exists(file_path):
                os.remove(file_path)
                logging.debug(f"Deleted cache file: {file_path}")
        except OSError as e:
            logging.error(f"Error deleting cache file {file_path}: {e}")

async def main():
    """Main function to set up directories and start servers."""
    # Create directories if they don't exist
    os.makedirs(CACHE_DIR, exist_ok=True)
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    logging.info(f"Cache directory: {os.path.abspath(CACHE_DIR)}")
    logging.info(f"Output directory: {os.path.abspath(OUTPUT_DIR)}")

    # Start the periodic video creation task
    video_task = asyncio.create_task(create_video_periodically())
    logging.info(f"Video creation task scheduled to run every {VIDEO_INTERVAL_SECONDS} seconds.")

    # Start the WebSocket server
    server_address = "0.0.0.0"
    logging.info(f"Starting WebSocket server on ws://{server_address}:{PORT}{PATH}")
    async with websockets.serve(save_frame, server_address, PORT, subprotocols=["binary"]): # Specify binary subprotocol if needed by client
        await asyncio.Future()  # Run forever

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logging.info("Server stopped manually.")