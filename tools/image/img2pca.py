#!/usr/bin/env python
import Image, sys, os.path

DEBUG = False

def serialize_rgb565(seq):
    res = ""
    for d in seq:
        res = res + chr(d>>8)
        res = res + chr(d&0xff)
    return res

def simple_rle(cumulated, current):
    if len(cumulated) == 0:
        return [ [current, 1] ]
    
    # Case 1: identical to last, increment
    if cumulated[-1][0] == current:
        cumulated[-1][1] = cumulated[-1][1] + 1
    
    # Case 2: not identical, open new
    else:
        cumulated.append( [current,  1] )
    
    return cumulated

TRIVIAL_CUTOFF = 5

def simplify_runs(cumulated, current):
    if len(cumulated) == 0:
        return [current]
    
    # Case 1: Make sure to output all not identical pixels
    if current[0] == False:
        if cumulated[-1][0] == False:
            # Merge in to previous one
            cumulated[-1] = [cumulated[-1][0],  cumulated[-1][1] + current[1]]
        else:
            # Simple append
            cumulated.append(current)
    
    # Case 2: if identical, and last was not identical:
    elif current[0] == True and cumulated[-1][0] == False:
        if current[1] > TRIVIAL_CUTOFF:
            # Suppressing this run saves more than it costs, do not output
            cumulated.append(current)
        else:
            # Suppressing this run would not save anything, merge it into the previous
            cumulated[-1] = [cumulated[-1][0],  cumulated[-1][1] + current[1]]
    
    # Don't know what to do, append it
    else:
        cumulated.append(current)

    return cumulated
    

class FileConverter(object):
    def __init__(self, filename):
        self.filename = filename
        im = Image.open(self.filename)
        
        self.width, self.height = im.size
        
        assert self.width < 127, "Can't handle images wider than 126 pixels yet"
        
        self.pixels_onscreen = map( lambda a: map(lambda b: None, range(self.height)), range(self.width) )
        
        self.outname = os.path.splitext(os.path.basename(self.filename))[0] + ".pca"
    
    def frames(self):
        curframe = 0
        while True:
            im = Image.open(self.filename)
            try:
                for i in range(curframe+1): im.seek(i)
            except EOFError:
                raise StopIteration
            frame = im.convert("RGB").rotate(180)
            yield frame
            curframe = curframe + 1
    
    def pixelstream(self, pixels):
        "Generate a stream of contiguous pixels, or repositioning instructions"
        
        skipped = True # At start of frame, explicitly place in 0,0 corner
        if DEBUG: print
        last_was_incomplete = False
        last_x_area = None
        
        for y in range(self.height):
            do_skip = True
            identical = []
            
            
            # Compare line to line on screen and skip if possible
            for x in range(self.width):
                # pack rgb565 pixel
                r, g, b = pixels[x, y]
                
                rgb565 =  ((r>>3)<<11) | ((g>>2)<<5) | ((b>>3)<<0)
                if self.pixels_onscreen[x][y] == rgb565:
                    identical.append(True)
                else:
                    identical.append(False)
                    do_skip = False
            
            if do_skip:
                skipped = True
                continue
            
            # Separate the "identical" mask into runs
            identical_runs = reduce(simple_rle, identical, [])
            
            # Simplify the runs and skip those that are too short
            do_runs = reduce(simplify_runs, identical_runs, [])
            
            if last_was_incomplete:
                skipped = True
                last_was_incomplete = False
            
            x = 0
            for run in do_runs:
                if run[0]:
                    # These are identical and don't need to be looked at, but increment x coordinate, also set skipped flag
                    x = x + run[1]
                    skipped = True
                else:
                    # Need to output these, prefix with window set if necessary
                    if skipped:
                        cords = (x, x + run[1] - 1, y, self.height-1)
                        if last_x_area != cords[:2]:
                            if DEBUG: print "[%i,%i,%i,%i]" % cords, 
                            yield (3,) + cords
                            last_x_area = cords[:2]
                        skipped = False
                        if cords[0] != 0 or cords[1] != self.width - 1:
                            last_was_incomplete = True
                    
                    if DEBUG: print "%i:" % y, 
                    for i in range(run[1]):
                        # send rgb565 packed pixel
                        r, g, b = pixels[x, y]
                        if DEBUG: print x, 
                        
                        rgb565 =  ((r>>3)<<11) | ((g>>2)<<5) | ((b>>3)<<0)
                        yield [0, [rgb565]]
                        self.pixels_onscreen[x][y] = rgb565
                        
                        x = x + 1
            if DEBUG: print
    
    def reduce_rle(self, cumulated, current):
        if len(cumulated) == 0:
            return [ current ]
        
        # Case a: current is a repositioning call, simply append
        if current[0] == 3:
            cumulated.append( current )
        # Case b: current is a pixel but last was not a pixel or rle, append
        elif not cumulated[-1][0] in (0, 1) and current[0] == 0:
            cumulated.append( current )
        # Case c: current is a pixel
        elif current[0] == 0:
            current = current[1][0]
            
            # Case 1: cumulated[-1] is not rle, current is not equal cumulated[-1][1][-1]: simple append
            if cumulated[-1][0] == 0 and current != cumulated[-1][1][-1]:
                cumulated[-1][1].append(current)
            
            # Case 2: cumulated[-1] is rle, current is equal to cumulated[-1][2]: simple increment
            elif cumulated[-1][0] == 1 and current == cumulated[-1][2]:
                cumulated[-1][1] = cumulated[-1][1] + 1
            
            # Case 3: cumulated[-1] is rle, current is not equal to cumulated[-1][2]: append literal
            elif cumulated[-1][0] == 1 and current != cumulated[-1][2]:
                cumulated.append( [0, [current]] )
            
            # Case 4: cumulated[-1] is not rle, current is equal to cumulated[-1][1][-1]: steal and append literal
            elif cumulated[-1][0] == 0 and current == cumulated[-1][1][-1]:
                cumulated[-1][1].pop()
                
                if len(cumulated[-1][1]) == 0:
                    # Special case: two RLE sequences after another -> pop empty literal sequence in between
                    cumulated.pop()
                
                cumulated.append( [1, 2, current] )
        
        return cumulated
    

    def generate_instructions(self, pixels, max_length):
        "Generate stream of pixel instructions, in chunks not longer than max_length bytes"
        
        result = []
        
        data = ""
        def append_data(da, d):
            if len(d) + len(da) >= max_length:
                result.append(da)
                da = ""
            da = da + d
            return da
        
        
        rle_sequence = reduce( self.reduce_rle, self.pixelstream(pixels), [] )
        
        for instruction in rle_sequence:
            if instruction[0] == 0:
                # Literal pixel data, needs 1 byte + 2* number of items, split as necessary
                while len(instruction[1]) > 0:
                    available = max_length - len(data) - 1 # in bytes
                    l = len(instruction[1]) # in 16-bit words
                    
                    if l*2 > available:
                        l = available/2
                    
                    data = append_data(data, chr(l) + serialize_rgb565(instruction[1][:l]) )
                    instruction[1] = instruction[1][l:]
            
            
            elif instruction[0] == 1:
                # RLE data, max of 0x3f repetitions per occurence, repeat as necessary
                while instruction[1] > 0:
                    rep = instruction[1]
                    if rep > 0x3F:
                        rep = 0x3F
                    
                    data = append_data(data, chr(0x80 + rep) + serialize_rgb565( [instruction[2]] ) )
                    instruction[1] = instruction[1] - rep
            
            elif instruction[0] == 3:
                data = append_data(data, "\xC0" + "".join(map(chr, instruction[1:])) )
        
        if len(data) > 0:
            result.append(data)
        
        return result

    def output_frame(self, frame):
        "Output all the necessary pixel instructions for one frame"
        pixels = frame.load()
        
        for data in self.generate_instructions(pixels, 254):
            
            if len(data) % 2 == 1:
                # Size unit for ppswitch is 16-bit, pad with 00
                data = data + "\x00"
            
            assert len(data) <= 254, "Internal error: ppswitch length field may not exceed 127*2 bytes"
            
            self.outfile.write(chr( len(data)/2  )) # Switch to pixel instruction mode
            self.outfile.write(data)

    def convert_file(self):
        self.outfile = file(self.outname, "wb" )
        
        # Header
        self.outfile.write("CANI\x02")
        self.outfile.write(chr(self.width))
        self.outfile.write(chr(self.height))
        
        for frame in self.frames():
            self.output_frame(frame)
            
            # One frame sent, pause 60ms
            self.outfile.write(chr(0x80 + 6))
    

if __name__ == "__main__":
    FileConverter(sys.argv[1]).convert_file()
    
