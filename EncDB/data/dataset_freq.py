import os
import operator
import csv,pickle
import random
import operator

class StreamingAnalyser:
    def __init__(self,dirFolder, threshold):

        self.dirPath = os.path.join(os.getcwd(),dirFolder);
        self.threshold = threshold
        self.freq = dict()

    def retrieve_keywords_from_files(self,fileId):
        fp = open(os.path.join(self.dirPath,str(fileId)))
        data = fp.read()
        keywords = data.split(",")

        fp.close()
        return keywords
    
    
    #function samples streaming data set by using the constructed and inverted index
    def dump_freq_by_threshold(self):
        for fileId in range(1,self.threshold+1):
            keyword_set = self.retrieve_keywords_from_files(fileId)
            for keyword in keyword_set:
                if len(keyword)==0:
                    print(fileId)
                if keyword in self.freq:
                    self.freq[keyword] +=1
                else:
                    self.freq[keyword] =1
            
        w_id_amount = 0
        #dump frequencies to file
        with open('freq-' + str(self.threshold) + '.csv', 'w') as csv_file:
            writer = csv.writer(csv_file)
            for w in sorted(self.freq, key=self.freq.get, reverse=True):
                writer.writerow([w, self.freq[w]])
                w_id_amount += self.freq[w]

        print("Completed " + str(w_id_amount) + " w-id pairs.")


if __name__ == '__main__':
    app = StreamingAnalyser(dirFolder="./small", threshold = int(30000*0.75))
    # app = StreamingAnalyser(dirFolder="./large", threshold = 1000000)
    # app = StreamingAnalyser(dirFolder="./enron", threshold = int(10000*0.25))
    app.dump_freq_by_threshold()