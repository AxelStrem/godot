from PIL import Image
import os

# List of icon/logo files to invert
files = [
    "icon.png",
    "icon_outlined.png",
    "logo.png",
    "logo_outlined.png",
    "main/app_icon.png",
    "main/splash.png",
    "platform/windows/godot.ico",
    "platform/windows/godot_console.ico"
]

# Output filenames
output_files = [
    "icon_inverted.png",
    "icon_outlined_inverted.png",
    "logo_inverted.png",
    "logo_outlined_inverted.png",
    "main/app_icon_inverted.png",
    "main/splash_inverted.png",
    "platform/windows/godot_inverted.ico",
    "platform/windows/godot_console_inverted.ico"
]

def invert_image(input_path, output_path):
    with Image.open(input_path) as img:
        # Handle .ico files which may contain multiple sizes
        if input_path.lower().endswith('.ico'):
            # For .ico files, we need to process all frames/sizes
            # Get the largest frame for conversion
            largest_size = 0
            largest_frame = 0
            
            try:
                frame_count = getattr(img, 'n_frames', 1)
                for i in range(frame_count):
                    img.seek(i)
                    size = img.width * img.height
                    if size > largest_size:
                        largest_size = size
                        largest_frame = i
                
                # Use the largest frame
                img.seek(largest_frame)
            except:
                # If we can't iterate frames, just use the current one
                pass
        
        # Convert to RGBA if not already
        img = img.convert("RGBA")
        # Invert colors
        r, g, b, a = img.split()
        rgb_image = Image.merge("RGB", (r, g, b))
        inverted_rgb = Image.eval(rgb_image, lambda x: 255 - x)
        inverted_img = Image.merge("RGBA", (*inverted_rgb.split(), a))
        
        # Save with appropriate format
        if output_path.lower().endswith('.ico'):
            # For .ico files, create multiple sizes
            sizes = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
            images = []
            for size in sizes:
                resized = inverted_img.resize(size, Image.Resampling.LANCZOS)
                images.append(resized)
            images[0].save(output_path, format='ICO', sizes=[(img.width, img.height) for img in images])
        else:
            inverted_img.save(output_path)
        
        print(f"Inverted: {input_path} -> {output_path}")

if __name__ == "__main__":
    base_dir = os.path.dirname(os.path.abspath(__file__))
    for src, dst in zip(files, output_files):
        src_path = os.path.join(base_dir, src)
        dst_path = os.path.join(base_dir, dst)
        if os.path.exists(src_path):
            invert_image(src_path, dst_path)
        else:
            print(f"File not found: {src_path}")
