/* 
 * Copyright (C) 2011 Department of Robotics Brain and Cognitive Sciences - Istituto Italiano di Tecnologia
 * Author: Carlo Ciliberto
 * email:  carlo.ciliberto@iit.it
 * Permission is granted to copy, distribute, and/or modify this program
 * under the terms of the GNU General Public License, version 2 or any
 * later version published by the Free Software Foundation.
 *
 * A copy of the license can be found at
 * http://www.robotcub.org/icub/license/gpl.txt
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details
*/

#include <yarp/os/Network.h>
#include <yarp/os/RFModule.h>
#include <yarp/os/Time.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/os/Semaphore.h>
#include <yarp/os/RpcClient.h>
#include <yarp/os/PortReport.h>

#include <yarp/sig/Vector.h>
#include <yarp/sig/Image.h>

#include <yarp/math/Math.h>
#include <yarp/math/Rand.h>

#include <highgui.h>
#include <cv.h>

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/features2d/features2d.hpp>

#include <iCub/boostMIL/ClassifierFactory.h>
#include <iCub/boostMIL/ClassifierInput.h>
#include <iCub/boostMIL/WeakClassifier.h>
#include <iCub/boostMIL/MILClassifier.h>
#include <iCub/boostMIL/OnlineBoost.h>

#include <stdio.h>
#include <string>
#include <deque>
#include <algorithm>
#include <vector>
#include <iostream>
#include <fstream>
#include <algorithm>

#include "SiftGPU_Extractor.h"
#include "DictionaryLearning.h"

using namespace std;
using namespace yarp;
using namespace yarp::os;
using namespace yarp::sig;
using namespace yarp::math;
using namespace iCub::boostMIL;

#define CMD_HELP                    VOCAB4('h','e','l','p')

#define CODE_MODE_SC                0
#define CODE_MODE_BOW               1


class SparseCoderPort: public BufferedPort<Image>
{
private:
    ResourceFinder                      &rf;

    bool                                verbose;

    vector<SiftGPU::SiftKeypoint>       keypoints;
    vector<float>                       descriptors;

    Port                                outPort;

    SiftGPU_Extractor                   siftGPU_extractor;

    int                                 grid_step;
    int                                 grid_scale;

    DictionaryLearning                  *sparse_coder;
    IplImage                            *ipl;

    Semaphore                           mutex;
    
    bool                                no_code;
    bool                                dump_sift;
    FILE                                *fout_sift;

    double                              rate;
    double                              last_read;

    int                                 code_mode;


    virtual void onRead(Image &img)
    {
        //read at specified rate
        if(Time::now()-last_read<rate)
            return;

        mutex.wait();
        if(ipl==NULL || ipl->width!=img.width() || ipl->height!=img.height())
        {
            if(ipl!=NULL)
            {
                cvReleaseImage(&ipl);
            }
            ipl=cvCreateImage(cvSize(img.width(),img.height()),IPL_DEPTH_8U,1);
            siftGPU_extractor.setDenseGrid(ipl,grid_step,grid_scale);
        }

        cvCvtColor((IplImage*)img.getIplImage(),ipl,CV_RGB2GRAY);

        //cvSmooth(ipl,ipl);  

        siftGPU_extractor.extractDenseSift(ipl,&keypoints,&descriptors);
        //siftGPU_extractor.extractSift(ipl,&keypoints,&descriptors);

        if(dump_sift)
        {
            for(int i=0; i<keypoints.size(); i++)
            {
                for(int j=0; j<128; j++)
                     fprintf(fout_sift,"%f ",descriptors[i*128+j]);
                fprintf(fout_sift,"\n");
            }
        }

        //code the input vector
        if(!no_code)
        {
            vector<Vector> features(keypoints.size());
            Vector coding;
            for(unsigned int i=0; i<keypoints.size(); i++)
            {
                features[i].resize(128);
                for(unsigned int j=0; j<128; j++)
                    features[i][j]=descriptors[i*128+j];
            }
            
            switch(code_mode)
            {
                case CODE_MODE_SC:
                {
                    sparse_coder->maxPooling(features,coding);
                    break;
                }
                case CODE_MODE_BOW:
                {
                    sparse_coder->bow(features,coding);
                    break;
                }
            }
            
            
            if(outPort.getOutputCount())
            {
                /*
                for(unsigned int i=0; i<keypoints.size(); i++)
                {   
                    int x = cvRound(keypoints[i].x);
                    int y = cvRound(keypoints[i].y);
                    cvCircle(img.getIplImage(),cvPoint(x,y),2,cvScalar(255),2);
                }
                outPort.write(img);s
                //*/
                outPort.write(coding);
            }
        }

        mutex.post();
    }


public:
   SparseCoderPort(ResourceFinder &_rf)
       :BufferedPort<Image>(),rf(_rf)
   {
        ipl=NULL;

        verbose=rf.check("verbose");

        grid_step=rf.check("grid_step",Value(8)).asInt();
        grid_scale=rf.check("grid_scale",Value(1)).asInt();

        string contextPath=rf.getContextPath().c_str();
        string dictionary_name=rf.check("dictionary_file",Value("dictionary_bow.ini")).asString().c_str();

        string dictionary_path=contextPath+"/"+dictionary_name;
        string dictionary_group=rf.check("dictionary_group",Value("DICTIONARY")).asString().c_str();

        no_code=rf.check("no_code");
        dump_sift=rf.check("dump_sift");

        if(dump_sift)
        {
            string sift_path=rf.check("dump_sift",Value("sift.txt")).asString().c_str();
            sift_path=contextPath+"/"+sift_path;
            string sift_write_mode=rf.check("append")?"a":"w";

            fout_sift=fopen(sift_path.c_str(),sift_write_mode.c_str());
        }

        rate=rf.check("rate",Value(0.0)).asDouble();
        last_read=0.0;

        sparse_coder=NULL;
        sparse_coder=new DictionaryLearning(dictionary_path,dictionary_group);

        string code_mode_string=rf.check("code_mode",Value("sc")).asString().c_str();
        
        //set all chars to lower case
        for(int i=0; i<code_mode_string.size(); i++)
            code_mode_string[i] = std::toupper((unsigned char)code_mode_string[i]);

        fprintf(stdout,"%s\n",code_mode_string.c_str());
        
        if(code_mode_string=="SC")
            code_mode=CODE_MODE_SC;
        if(code_mode_string=="BOW")
            code_mode=CODE_MODE_BOW;

        string name=rf.find("name").asString().c_str();

        outPort.open(("/"+name+"/code:o").c_str());
        BufferedPort<Image>::useCallback();
   }

   virtual void close()
   {
        mutex.wait();
        if(ipl!=NULL)
            cvReleaseImage(&ipl);

        if(dump_sift)
            fclose(fout_sift);

        //some closure :P
        if(sparse_coder!=NULL)
            delete sparse_coder;

        outPort.close();
        BufferedPort<Image>::close();
   }


   bool execReq(const Bottle &command, Bottle &reply)
   {
       switch(command.get(0).asVocab())
       {
           case(CMD_HELP):
           {
                reply.clear();
                reply.addString("There's no help in this life. You oughta do everything on your own");
                
                return true;
           }

           default:
               return false;
       }
   }


};


class SparseCoderModule: public RFModule
{
protected:
    SparseCoderPort         *scPort;
    Port                    rpcPort;

public:
    SparseCoderModule()
    {
        scPort=NULL;
    }

    virtual bool configure(ResourceFinder &rf)
    {
        string name=rf.find("name").asString().c_str();

        Time::turboBoost();

        scPort=new SparseCoderPort(rf);
        scPort->open(("/"+name+"/img:i").c_str());
        rpcPort.open(("/"+name+"/rpc").c_str());
        attach(rpcPort);

        return true;
    }

    virtual bool close()
    {
        if(scPort!=NULL)
        {
            scPort->interrupt();
            scPort->close();
            delete scPort;
        }
        
        rpcPort.interrupt();
        rpcPort.close();

        return true;
    }

    virtual bool respond(const Bottle &command, Bottle &reply)
    {
        if(scPort->execReq(command,reply))
            return true;
        else
            return RFModule::respond(command,reply);
    }

    virtual double getPeriod()    { return 1.0;  }
    virtual bool   updateModule()
    {
        //scPort->update();

        return true;
    }

};




int main(int argc, char *argv[])
{
   Network yarp;

   if (!yarp.checkNetwork())
       return -1;

   ResourceFinder rf;
   rf.setVerbose(true);
   rf.setDefaultContext("onTheFlyRecognition/conf");   
   rf.setDefaultConfigFile("sparseCoder.ini");
   rf.configure("ICUB_ROOT",argc,argv);
   rf.setDefault("name","sparseCoder");
   SparseCoderModule mod;

   return mod.runModule(rf);
}

